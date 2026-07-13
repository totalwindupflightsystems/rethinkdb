// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/ivfflat.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <random>
#include <stdexcept>
#include <unordered_set>

#include "containers/archive/stl_types.hpp"
#include "rdb_protocol/vector_distance.hpp"
#include "rpc/serialize_macros.hpp"

namespace ql {

// ── Construction ──

ivfflat_index_t::ivfflat_index_t(int nlist, int nprobe, size_t dim,
                                 metric_t metric)
    : nlist_(nlist),
      nprobe_(nprobe),
      dim_(dim),
      metric_(metric),
      trained_(false),
      total_count_(0) {
    guarantee(nlist_ >= 1, "IVFFlat nlist must be >= 1");
    guarantee(nprobe_ >= 1, "IVFFlat nprobe must be >= 1");
    guarantee(nprobe_ <= nlist_,
              "IVFFlat nprobe must be <= nlist");
}

// ── Distance computation ──

double ivfflat_index_t::compute_distance(const double *a,
                                          const double *b) const {
    switch (metric_) {
    case metric_t::L2:
        return l2_distance(a, b, dim_);
    case metric_t::COSINE:
        return cosine_distance(a, b, dim_);
    case metric_t::INNER_PRODUCT:
        return inner_product_distance(a, b, dim_);
    default:
        unreachable();
    }
}

// ── Centroid lookup ──

std::pair<int, double> ivfflat_index_t::find_nearest_centroid(
        const double *query) const {
    guarantee(trained_, "IVFFlat index must be trained before querying");
    guarantee(!centroids_.empty(), "IVFFlat centroids are empty");

    int best_idx = -1;
    double best_dist = std::numeric_limits<double>::max();

    for (size_t i = 0; i < centroids_.size(); ++i) {
        double dist = compute_distance(query, centroids_[i].data());
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = static_cast<int>(i);
        }
    }
    return {best_idx, best_dist};
}

std::vector<std::pair<int, double>>
ivfflat_index_t::find_nearest_centroids(
        const double *query, int nprobe) const {
    guarantee(trained_, "IVFFlat index must be trained before querying");

    // Compute distance to all centroids, keep top nprobe
    std::vector<std::pair<double, int>> dists;
    dists.reserve(centroids_.size());
    for (size_t i = 0; i < centroids_.size(); ++i) {
        double dist = compute_distance(query, centroids_[i].data());
        dists.emplace_back(dist, static_cast<int>(i));
    }

    // Partial sort to get top nprobe
    int actual_probe = std::min(nprobe, static_cast<int>(dists.size()));
    std::partial_sort(dists.begin(),
                       dists.begin() + actual_probe,
                       dists.end());

    std::vector<std::pair<int, double>> result;
    result.reserve(actual_probe);
    for (int i = 0; i < actual_probe; ++i) {
        result.emplace_back(dists[i].second, dists[i].first);
    }
    return result;
}

// ── K-means clustering ──

std::vector<std::vector<double>> ivfflat_index_t::kmeans(
        const std::vector<std::vector<double>> &data,
        int k, int max_iterations) const {
    guarantee(!data.empty(), "K-means requires at least one data point");
    guarantee(k >= 1, "K-means requires k >= 1");

    size_t n = data.size();

    // Effective k cannot exceed data size
    int effective_k = std::min(k, static_cast<int>(n));

    // Initialize centroids with random data points (Forgy method)
    std::vector<std::vector<double>> centroids(effective_k);
    std::vector<size_t> indices(n);
    for (size_t i = 0; i < n; ++i) indices[i] = i;

    static thread_local std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    std::shuffle(indices.begin(), indices.end(), gen);

    for (int i = 0; i < effective_k; ++i) {
        centroids[i] = data[indices[i]];
    }

    // Assignment array
    std::vector<int> assignments(n);

    for (int iter = 0; iter < max_iterations; ++iter) {
        bool changed = false;

        // Assignment step: assign each point to nearest centroid
        for (size_t i = 0; i < n; ++i) {
            int best_c = 0;
            double best_dist = compute_distance(
                data[i].data(), centroids[0].data());
            for (int c = 1; c < effective_k; ++c) {
                double dist = compute_distance(
                    data[i].data(), centroids[c].data());
                if (dist < best_dist) {
                    best_dist = dist;
                    best_c = c;
                }
            }
            if (assignments[i] != best_c) {
                assignments[i] = best_c;
                changed = true;
            }
        }

        // Update step: recompute centroids as mean of assigned points
        std::vector<std::vector<double>> new_centroids(
            effective_k, std::vector<double>(dim_, 0.0));
        std::vector<size_t> counts(effective_k, 0);

        for (size_t i = 0; i < n; ++i) {
            int c = assignments[i];
            for (size_t d = 0; d < dim_; ++d) {
                new_centroids[c][d] += data[i][d];
            }
            counts[c]++;
        }

        for (int c = 0; c < effective_k; ++c) {
            if (counts[c] > 0) {
                for (size_t d = 0; d < dim_; ++d) {
                    new_centroids[c][d] /= static_cast<double>(counts[c]);
                }
            } else {
                // Empty cluster: keep old centroid
                new_centroids[c] = centroids[c];
            }
        }

        centroids = std::move(new_centroids);

        if (!changed) break;
    }

    return centroids;
}

// ── Training ──

void ivfflat_index_t::train(
        const std::vector<std::vector<double>> &training_vectors,
        int max_iterations) {
    guarantee(dim_ > 0, "IVFFlat dim must be set before training");
    guarantee(!training_vectors.empty(),
              "Training requires at least one vector");

    // Verify dimensions
    for (size_t i = 0; i < training_vectors.size(); ++i) {
        guarantee(training_vectors[i].size() == dim_,
                  "All training vectors must have dim=%zu (vector %zu has %zu)",
                  dim_, i, training_vectors[i].size());
    }

    // Run k-means
    centroids_ = kmeans(training_vectors, nlist_, max_iterations);

    // Initialize inverted lists
    lists_.clear();
    lists_.resize(centroids_.size());
    total_count_ = 0;
    trained_ = true;
}

// ── Insert ──

void ivfflat_index_t::insert(const double *vec, const store_key_t &key) {
    guarantee(trained_, "IVFFlat index must be trained before inserting");
    guarantee(dim_ > 0, "IVFFlat dim must be set before inserting");

    // Find nearest centroid
    auto [centroid_idx, _] = find_nearest_centroid(vec);

    // Add to inverted list
    auto &lst = lists_[centroid_idx];
    lst.vectors.emplace_back(vec, vec + dim_);
    lst.keys.push_back(key);
    lst.deleted.push_back(false);
    total_count_++;
}

// ── Search ──

std::vector<std::pair<double, store_key_t>>
ivfflat_index_t::search_knn(const double *query, int k, int nprobe) const {
    if (k <= 0 || total_count_ == 0) return {};

    int effective_nprobe = (nprobe < 1) ? nprobe_ : nprobe;
    effective_nprobe = std::min(effective_nprobe,
                                 static_cast<int>(centroids_.size()));

    // Find nearest centroids to probe
    auto nearest_centroids = find_nearest_centroids(query, effective_nprobe);

    // Collect candidates from probed inverted lists
    // Max-heap to keep top-k results
    auto cmp = [](const std::pair<double, store_key_t> &a,
                   const std::pair<double, store_key_t> &b) {
        return a.first < b.first;
    };
    std::priority_queue<std::pair<double, store_key_t>,
                        std::vector<std::pair<double, store_key_t>>,
                        decltype(cmp)> heap(cmp);

    for (const auto &[cidx, _] : nearest_centroids) {
        const auto &lst = lists_[cidx];
        for (size_t i = 0; i < lst.vectors.size(); ++i) {
            if (lst.deleted[i]) continue;

            double dist = compute_distance(query, lst.vectors[i].data());

            if (static_cast<int>(heap.size()) < k) {
                heap.emplace(dist, lst.keys[i]);
            } else if (dist < heap.top().first) {
                heap.pop();
                heap.emplace(dist, lst.keys[i]);
            }
        }
    }

    // Collect and reverse (heap returns descending, we want ascending)
    std::vector<std::pair<double, store_key_t>> results;
    results.reserve(heap.size());
    while (!heap.empty()) {
        results.push_back(heap.top());
        heap.pop();
    }
    std::reverse(results.begin(), results.end());
    return results;
}

// ── Remove ──

void ivfflat_index_t::remove(const store_key_t &key) {
    for (auto &lst : lists_) {
        for (size_t i = 0; i < lst.keys.size(); ++i) {
            if (lst.keys[i] == key && !lst.deleted[i]) {
                lst.deleted[i] = true;
                total_count_--;
                return;
            }
        }
    }
}

// ── Size ──

size_t ivfflat_index_t::size() const {
    return total_count_;
}

// ── Serialization ──

template <cluster_version_t W>
void serialize(write_message_t *wm, const ivfflat_index_t &idx) {
    serialize<W>(wm, idx.nlist_);
    serialize<W>(wm, idx.nprobe_);
    serialize<W>(wm, idx.dim_);
    serialize<W>(wm, static_cast<int>(idx.metric_));
    serialize<W>(wm, idx.trained_);
    serialize<W>(wm, idx.total_count_);

    // Centroids
    size_t centroids_sz = idx.centroids_.size();
    serialize<W>(wm, centroids_sz);
    for (const auto &c : idx.centroids_) {
        size_t c_sz = c.size();
        serialize<W>(wm, c_sz);
        for (double val : c) {
            serialize<W>(wm, val);
        }
    }

    // Inverted lists
    size_t lists_sz = idx.lists_.size();
    serialize<W>(wm, lists_sz);
    for (const auto &lst : idx.lists_) {
        // vectors
        size_t vec_sz = lst.vectors.size();
        serialize<W>(wm, vec_sz);
        for (const auto &v : lst.vectors) {
            size_t v_sz = v.size();
            serialize<W>(wm, v_sz);
            for (double val : v) {
                serialize<W>(wm, val);
            }
        }
        // keys
        size_t key_sz = lst.keys.size();
        serialize<W>(wm, key_sz);
        for (const auto &k : lst.keys) {
            serialize<W>(wm, k);
        }
        // deleted
        size_t del_sz = lst.deleted.size();
        serialize<W>(wm, del_sz);
        for (bool d : lst.deleted) {
            serialize<W>(wm, d);
        }
    }
}

template <cluster_version_t W>
archive_result_t deserialize(read_stream_t *s, ivfflat_index_t *idx) {
    archive_result_t res = deserialize<W>(s, &idx->nlist_);
    if (bad(res)) return res;
    res = deserialize<W>(s, &idx->nprobe_);
    if (bad(res)) return res;
    res = deserialize<W>(s, &idx->dim_);
    if (bad(res)) return res;
    int metric_int;
    res = deserialize<W>(s, &metric_int);
    if (bad(res)) return res;
    idx->metric_ = static_cast<ivfflat_index_t::metric_t>(metric_int);
    res = deserialize<W>(s, &idx->trained_);
    if (bad(res)) return res;
    res = deserialize<W>(s, &idx->total_count_);
    if (bad(res)) return res;

    // Centroids
    size_t centroids_sz;
    res = deserialize<W>(s, &centroids_sz);
    if (bad(res)) return res;
    idx->centroids_.clear();
    idx->centroids_.reserve(centroids_sz);
    for (size_t i = 0; i < centroids_sz; ++i) {
        size_t c_sz;
        res = deserialize<W>(s, &c_sz);
        if (bad(res)) return res;
        std::vector<double> centroid(c_sz);
        for (size_t j = 0; j < c_sz; ++j) {
            res = deserialize<W>(s, &centroid[j]);
            if (bad(res)) return res;
        }
        idx->centroids_.push_back(std::move(centroid));
    }

    // Inverted lists
    size_t lists_sz;
    res = deserialize<W>(s, &lists_sz);
    if (bad(res)) return res;
    idx->lists_.clear();
    idx->lists_.reserve(lists_sz);
    for (size_t i = 0; i < lists_sz; ++i) {
        ivfflat_index_t::inverted_list_t lst;

        // vectors
        size_t vec_sz;
        res = deserialize<W>(s, &vec_sz);
        if (bad(res)) return res;
        lst.vectors.reserve(vec_sz);
        for (size_t j = 0; j < vec_sz; ++j) {
            size_t v_sz;
            res = deserialize<W>(s, &v_sz);
            if (bad(res)) return res;
            std::vector<double> v(v_sz);
            for (size_t k = 0; k < v_sz; ++k) {
                res = deserialize<W>(s, &v[k]);
                if (bad(res)) return res;
            }
            lst.vectors.push_back(std::move(v));
        }

        // keys
        size_t key_sz;
        res = deserialize<W>(s, &key_sz);
        if (bad(res)) return res;
        lst.keys.reserve(key_sz);
        for (size_t j = 0; j < key_sz; ++j) {
            store_key_t key;
            res = deserialize<W>(s, &key);
            if (bad(res)) return res;
            lst.keys.push_back(std::move(key));
        }

        // deleted
        size_t del_sz;
        res = deserialize<W>(s, &del_sz);
        if (bad(res)) return res;
        lst.deleted.reserve(del_sz);
        for (size_t j = 0; j < del_sz; ++j) {
            bool d;
            res = deserialize<W>(s, &d);
            if (bad(res)) return res;
            lst.deleted.push_back(d);
        }

        idx->lists_.push_back(std::move(lst));
    }

    return archive_result_t::SUCCESS;
}

INSTANTIATE_SERIALIZABLE_SINCE_v2_4(ivfflat_index_t);

}  // namespace ql
