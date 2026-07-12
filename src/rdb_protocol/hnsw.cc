// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/hnsw.hpp"

#include <algorithm>
#include <cmath>
#include <queue>
#include <random>
#include <set>
#include <stdexcept>
#include <unordered_set>

#include "containers/archive/stl_types.hpp"
#include "rdb_protocol/vector_distance.hpp"
#include "rpc/serialize_macros.hpp"

namespace ql {

// ── Construction ──

hnsw_graph_t::hnsw_graph_t(int M, int ef_construction, size_t dim,
                           metric_t metric)
    : M_(M),
      M_max0_(2 * M),
      ef_construction_(ef_construction),
      dim_(dim),
      ml_(-1),
      metric_(metric),
      entry_point_(-1) {
    guarantee(M_ >= 2, "HNSW M must be >= 2");
    guarantee(M_max0_ >= 2, "HNSW M_max0 must be >= 2");
    guarantee(ef_construction_ >= 1, "HNSW ef_construction must be >= 1");
}

// ── Level sampling ──

int hnsw_graph_t::sample_level() const {
    // m_L = 1 / ln(M) normalizes the exponential distribution
    // so that higher M → lower expected level.
    static thread_local std::random_device rd;
    static thread_local std::mt19937 gen(rd());
    static thread_local std::uniform_real_distribution<double> dist(0.0, 1.0);

    double r = dist(gen);
    if (r <= 0.0) r = std::numeric_limits<double>::min();
    double m_L = 1.0 / std::log(static_cast<double>(M_));
    return static_cast<int>(std::floor(-std::log(r) * m_L));
}

// ── Distance computation ──

double hnsw_graph_t::compute_distance(const double *a, const double *b) const {
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

// ── Greedy single-path search ──

int hnsw_graph_t::greedy_search_layer(
        const double *query, int ep, int layer) const {
    int current = ep;
    double current_dist = compute_distance(query, nodes_[current].vec.data());

    bool changed = true;
    while (changed) {
        changed = false;
        for (int neighbor : nodes_[current].layers_[layer]) {
            double dist = compute_distance(query, nodes_[neighbor].vec.data());
            if (dist < current_dist) {
                current = neighbor;
                current_dist = dist;
                changed = true;
            }
        }
    }
    return current;
}

// ── Multi-candidate layer search ──

std::vector<std::pair<double, int>> hnsw_graph_t::search_layer(
        const double *query,
        const std::vector<int> &entry_points,
        int layer, int ef) const {
    if (nodes_.empty()) return {};

    // visited set: track nodes already examined
    std::unordered_set<int> visited;

    // Result set: max-heap of (distance, node_id), capacity ef.
    // We use a custom comparator so the top is the *farthest* element.
    auto cmp_max = [](const std::pair<double, int> &a,
                       const std::pair<double, int> &b) {
        return a.first < b.first;
    };
    std::priority_queue<std::pair<double, int>,
                        std::vector<std::pair<double, int>>,
                        decltype(cmp_max)> result(cmp_max);

    // Candidate frontier: min-heap, pop the *closest* first.
    auto cmp_min = [](const std::pair<double, int> &a,
                       const std::pair<double, int> &b) {
        return a.first > b.first;
    };
    std::priority_queue<std::pair<double, int>,
                        std::vector<std::pair<double, int>>,
                        decltype(cmp_min)> frontier(cmp_min);

    // Initialize with entry points
    for (int ep : entry_points) {
        double dist = compute_distance(query, nodes_[ep].vec.data());
        result.push({dist, ep});
        frontier.push({dist, ep});
        visited.insert(ep);
    }

    while (!frontier.empty()) {
        auto [dist_c, c] = frontier.top();
        frontier.pop();

        // Get the farthest element in the result set
        double farthest_dist = result.top().first;

        // If the nearest candidate is farther than the farthest result,
        // all remaining candidates would also be worse.
        if (dist_c > farthest_dist) break;

        // Examine neighbors
        for (int neighbor : nodes_[c].layers_[layer]) {
            if (visited.count(neighbor)) continue;
            visited.insert(neighbor);

            double dist_n = compute_distance(query, nodes_[neighbor].vec.data());

            if (static_cast<int>(result.size()) < ef || dist_n < farthest_dist) {
                result.push({dist_n, neighbor});
                frontier.push({dist_n, neighbor});

                // Keep result capped at ef
                if (static_cast<int>(result.size()) > ef) {
                    result.pop();
                }
                // Update farthest_dist
                if (!result.empty()) {
                    farthest_dist = result.top().first;
                }
            }
        }
    }

    // Collect and sort results ascending by distance
    std::vector<std::pair<double, int>> sorted_results;
    sorted_results.reserve(result.size());
    while (!result.empty()) {
        sorted_results.push_back(result.top());
        result.pop();
    }
    // The max-heap gives us results in descending order; reverse to ascending.
    std::reverse(sorted_results.begin(), sorted_results.end());
    return sorted_results;
}

// ── Simple neighbor selection ──

std::vector<int> hnsw_graph_t::select_neighbors(
        const double * /*query*/,
        const std::vector<std::pair<double, int>> &candidates,
        int M_max) const {
    // Simple heuristic: return the M_max closest candidates by distance.
    // Candidates are already sorted ascending by distance.
    std::vector<int> selected;
    selected.reserve(std::min(static_cast<size_t>(M_max), candidates.size()));
    for (size_t i = 0; i < candidates.size() && selected.size() < static_cast<size_t>(M_max); ++i) {
        selected.push_back(candidates[i].second);
    }
    return selected;
}

// ── Insert ──

void hnsw_graph_t::insert(const double *vec, const store_key_t &key) {
    guarantee(dim_ > 0, "HNSW graph dimension must be set before inserting");

    int new_id = static_cast<int>(nodes_.size());
    int l = sample_level();

    node_t new_node;
    new_node.vec.assign(vec, vec + dim_);
    new_node.layers_.resize(l + 1);
    new_node.primary_key = key;
    new_node.deleted = false;
    nodes_.push_back(std::move(new_node));

    // First node: set as entry point
    if (entry_point_ < 0) {
        entry_point_ = new_id;
        ml_ = l;
        return;
    }

    // Phase 1: traverse from top to l+1 using greedy descent
    int current_ep = entry_point_;
    for (int layer = ml_; layer > l; --layer) {
        current_ep = greedy_search_layer(vec, current_ep, layer);
    }

    // Phase 2: insert into layers l down to 0
    int max_layer = std::min(l, ml_);
    for (int layer = max_layer; layer >= 0; --layer) {
        int M_max = (layer == 0) ? M_max0_ : M_;

        std::vector<int> entry_pts;
        entry_pts.push_back(current_ep);

        auto candidates = search_layer(vec, entry_pts, layer, ef_construction_);
        auto neighbors = select_neighbors(vec, candidates, M_max);

        // Add bidirectional edges
        for (int nbr : neighbors) {
            // forward edge
            nodes_[new_id].layers_[layer].push_back(nbr);
            // reverse edge
            nodes_[nbr].layers_[layer].push_back(new_id);

            // Prune nbr's connections if over capacity
            if (static_cast<int>(nodes_[nbr].layers_[layer].size()) > M_max) {
                // Recompute distances from nbr to all its neighbors
                std::vector<std::pair<double, int>> nbr_candidates;
                nbr_candidates.reserve(nodes_[nbr].layers_[layer].size());
                for (int nn : nodes_[nbr].layers_[layer]) {
                    double d = compute_distance(
                        nodes_[nbr].vec.data(), nodes_[nn].vec.data());
                    nbr_candidates.push_back({d, nn});
                }
                std::sort(nbr_candidates.begin(), nbr_candidates.end());
                // Keep the M_max closest
                nodes_[nbr].layers_[layer].clear();
                for (size_t j = 0;
                     j < nbr_candidates.size() && static_cast<int>(j) < M_max;
                     ++j) {
                    nodes_[nbr].layers_[layer].push_back(nbr_candidates[j].second);
                }
            }
        }
    }

    // Update entry point if we added a higher-level node
    if (l > ml_) {
        ml_ = l;
        entry_point_ = new_id;
    }
}

// ── k-NN Search ──

std::vector<std::pair<double, store_key_t>> hnsw_graph_t::search_knn(
        const double *query, int k, int ef_search) const {
    if (nodes_.empty() || k <= 0) return {};

    int current_ep = entry_point_;

    // Traverse from top layer down to layer 1 via greedy descent
    for (int layer = ml_; layer > 0; --layer) {
        current_ep = greedy_search_layer(query, current_ep, layer);
    }

    // Search at layer 0 with ef = max(ef_search, k)
    int ef = std::max(ef_search, k);
    std::vector<int> entry_pts;
    entry_pts.push_back(current_ep);

    auto candidates = search_layer(query, entry_pts, 0, ef);

    // Collect top-k non-deleted results
    std::vector<std::pair<double, store_key_t>> results;
    results.reserve(k);
    for (const auto &[dist, node_id] : candidates) {
        if (!nodes_[node_id].deleted) {
            results.emplace_back(dist, nodes_[node_id].primary_key);
            if (static_cast<int>(results.size()) >= k) break;
        }
    }
    return results;
}

// ── Remove (tombstone) ──

void hnsw_graph_t::remove(const store_key_t &key) {
    for (auto &node : nodes_) {
        if (node.primary_key == key) {
            node.deleted = true;
            return;
        }
    }
}

// ── Serialization ──

template <cluster_version_t W>
void serialize(write_message_t *wm, const hnsw_graph_t &g) {
    serialize<W>(wm, g.M_);
    serialize<W>(wm, g.M_max0_);
    serialize<W>(wm, g.ef_construction_);
    serialize<W>(wm, g.dim_);
    serialize<W>(wm, g.ml_);
    serialize<W>(wm, static_cast<int>(g.metric_));
    serialize<W>(wm, g.entry_point_);
    // Manually serialize nodes (field-by-field for safety)
    size_t node_count = g.nodes_.size();
    serialize<W>(wm, node_count);
    for (const auto &n : g.nodes_) {
        // vec
        size_t vec_sz = n.vec.size();
        serialize<W>(wm, vec_sz);
        for (double val : n.vec) {
            serialize<W>(wm, val);
        }
        // layers_
        size_t layer_count = n.layers_.size();
        serialize<W>(wm, layer_count);
        for (const auto &layer : n.layers_) {
            size_t nbr_count = layer.size();
            serialize<W>(wm, nbr_count);
            for (int nbr : layer) {
                serialize<W>(wm, nbr);
            }
        }
        // primary_key
        serialize<W>(wm, n.primary_key);
        // deleted
        serialize<W>(wm, n.deleted);
    }
}

template <cluster_version_t W>
archive_result_t deserialize(read_stream_t *s, hnsw_graph_t *g) {
    archive_result_t res = deserialize<W>(s, &g->M_);
    if (bad(res)) return res;
    res = deserialize<W>(s, &g->M_max0_);
    if (bad(res)) return res;
    res = deserialize<W>(s, &g->ef_construction_);
    if (bad(res)) return res;
    res = deserialize<W>(s, &g->dim_);
    if (bad(res)) return res;
    res = deserialize<W>(s, &g->ml_);
    if (bad(res)) return res;
    int metric_int;
    res = deserialize<W>(s, &metric_int);
    if (bad(res)) return res;
    g->metric_ = static_cast<hnsw_graph_t::metric_t>(metric_int);
    res = deserialize<W>(s, &g->entry_point_);
    if (bad(res)) return res;

    size_t node_count;
    res = deserialize<W>(s, &node_count);
    if (bad(res)) return res;
    g->nodes_.clear();
    g->nodes_.reserve(node_count);
    for (size_t i = 0; i < node_count; ++i) {
        hnsw_graph_t::node_t node;
        // vec
        size_t vec_sz;
        res = deserialize<W>(s, &vec_sz);
        if (bad(res)) return res;
        node.vec.resize(vec_sz);
        for (size_t j = 0; j < vec_sz; ++j) {
            res = deserialize<W>(s, &node.vec[j]);
            if (bad(res)) return res;
        }
        // layers_
        size_t layer_count;
        res = deserialize<W>(s, &layer_count);
        if (bad(res)) return res;
        node.layers_.resize(layer_count);
        for (size_t j = 0; j < layer_count; ++j) {
            size_t nbr_count;
            res = deserialize<W>(s, &nbr_count);
            if (bad(res)) return res;
            node.layers_[j].resize(nbr_count);
            for (size_t k = 0; k < nbr_count; ++k) {
                res = deserialize<W>(s, &node.layers_[j][k]);
                if (bad(res)) return res;
            }
        }
        // primary_key
        res = deserialize<W>(s, &node.primary_key);
        if (bad(res)) return res;
        // deleted
        res = deserialize<W>(s, &node.deleted);
        if (bad(res)) return res;

        g->nodes_.push_back(std::move(node));
    }
    return archive_result_t::SUCCESS;
}

INSTANTIATE_SERIALIZABLE_SINCE_v2_4(hnsw_graph_t);

}  // namespace ql
