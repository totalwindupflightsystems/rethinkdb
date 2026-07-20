// Copyright 2026 RethinkDB, all rights reserved.
//
// VECTOR-8: Correctness tests for the vector index pipeline.
//
// Two layers of correctness are exercised here:
//
//  1. HNSW approximate recall — we build a small known dataset of 3-D
//     vectors, build both a brute-force exact KNN ground truth and an
//     HNSW index, then measure how many of the top-k neighbours from
//     HNSW overlap with the ground truth. The index is approximate, so
//     we assert recall >= 95 percent at a generous ef_search setting.
//
//  2. Metric consistency — identity, orthogonality, normalisation and
//     symmetry invariants for the L2 / cosine / inner-product distance
//     functions used by the indexes.
//
// These tests are stateless (no live server, no serialisation round-trip
// beyond the existing HNSW/IVFFlat tests in hnsw_test.cc and
// ivfflat_test.cc). They use the public C++ API of the indexes.

#include "rdb_protocol/hnsw.hpp"
#include "rdb_protocol/ivfflat.hpp"
#include "rdb_protocol/vector_distance.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <random>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "btree/keys.hpp"
#include "unittest/gtest.hpp"

namespace unittest {

namespace {

constexpr double kEpsilon = 1e-9;
constexpr size_t kDim = 3;

// ── Helpers ──────────────────────────────────────────────────────────

// Brute-force exact KNN using the supplied distance function. Used to
// build ground truth for the HNSW recall test.
std::vector<std::pair<double, std::string>> brute_force_knn(
    const std::vector<std::pair<std::vector<double>, std::string>> &dataset,
    const std::vector<double> &query,
    size_t k) {
    using pair_t = std::pair<double, std::string>;
    std::vector<pair_t> all;
    all.reserve(dataset.size());
    for (const auto &entry : dataset) {
        double d = ql::l2_distance(query.data(), entry.first.data(), kDim);
        all.emplace_back(d, entry.second);
    }
    std::sort(all.begin(), all.end(),
              [](const pair_t &a, const pair_t &b) { return a.first < b.first; });
    if (k > all.size()) k = all.size();
    return std::vector<pair_t>(all.begin(), all.begin() + k);
}

// Recall@k: fraction of HNSW top-k keys that appear in the exact top-k
// keys. Computed as |intersection| / k.
double recall_at_k(const std::vector<std::pair<double, store_key_t>> &approx,
                   const std::vector<std::pair<double, std::string>> &exact) {
    std::unordered_set<std::string> exact_keys;
    exact_keys.reserve(exact.size());
    for (const auto &p : exact) exact_keys.insert(p.second);

    size_t hits = 0;
    for (const auto &p : approx) {
        std::string key_str(
            reinterpret_cast<const char *>(p.second.contents()),
            p.second.size());
        if (exact_keys.count(key_str) > 0) ++hits;
    }
    return static_cast<double>(hits) / static_cast<double>(exact.size());
}

}  // namespace

// ── Metric consistency ───────────────────────────────────────────────

TEST(VectorCorrectnessTest, CosineDistanceOfIdenticalVectorsIsZero) {
    std::vector<double> v = {0.3, -0.7, 1.2};
    double d = ql::cosine_distance(v.data(), v.data(), v.size());
    EXPECT_NEAR(0.0, d, kEpsilon);
}

TEST(VectorCorrectnessTest, CosineDistanceOfOrthogonalVectorsIsOne) {
    // e_x vs e_y in 3-D — dot product 0, so cosine distance is 1 - 0 = 1.
    std::vector<double> a = {1.0, 0.0, 0.0};
    std::vector<double> b = {0.0, 1.0, 0.0};
    double d = ql::cosine_distance(a.data(), b.data(), a.size());
    EXPECT_NEAR(1.0, d, kEpsilon);
}

TEST(VectorCorrectnessTest, InnerProductOfIdenticalNormalizedVectorsIsOne) {
    // inner_product_distance returns the negation of the dot product so
    // that "nearest" = smallest distance for max-similarity search.
    // For two identical unit vectors, the dot product is 1.0, so the
    // distance must be exactly -1.0.
    std::vector<double> a = {1.0, 0.0, 0.0};
    std::vector<double> b = {1.0, 0.0, 0.0};
    double dist = ql::inner_product_distance(a.data(), b.data(), a.size());
    EXPECT_NEAR(-1.0, dist, kEpsilon);
}

TEST(VectorCorrectnessTest, L2DistanceIsSymmetric) {
    // dist(a, b) == dist(b, a) for arbitrary vectors.
    std::vector<double> a = {0.5, -1.3, 2.7};
    std::vector<double> b = {1.2, 0.8, -0.4};
    double dab = ql::l2_distance(a.data(), b.data(), a.size());
    double dba = ql::l2_distance(b.data(), a.data(), b.size());
    EXPECT_NEAR(dab, dba, kEpsilon);

    // And for cosine — the metric used by the HNSW cosine index path.
    double cab = ql::cosine_distance(a.data(), b.data(), a.size());
    double cba = ql::cosine_distance(b.data(), a.data(), b.size());
    EXPECT_NEAR(cab, cba, kEpsilon);
}

// ── HNSW recall ──────────────────────────────────────────────────────

TEST(VectorCorrectnessTest, HNSWRecallOnSmallDataset) {
    // HNSW recall test: build a clustered 100-vector dataset in 3-D
    // and verify the index finds >= 95 percent of the true top-k
    // neighbours (averaged over 20 queries).
    //
    // The dataset has 10 well-separated cluster centres with 10 points
    // each. Queries are placed near a random cluster centre; with
    // ef_search much larger than k and a 10x larger than dataset-size,
    // the search has a lot of slack and reliably converges to the
    // exact top-k from within the cluster.
    //
    // The dataset and queries are deterministic (fixed seed). HNSW's
    // per-node level sampling uses std::random_device and is therefore
    // non-deterministic across runs, but a high ef_search keeps recall
    // robust against that variance.

    constexpr size_t kDatasetSize = 100;
    constexpr size_t kClusters = 10;
    constexpr size_t kPerCluster = kDatasetSize / kClusters;
    constexpr size_t kQueries = 20;
    constexpr size_t k = 5;
    constexpr int kEfSearch = 1000;
    constexpr double kMinRecall = 0.95;

    std::mt19937 rng(0xC0FFEEu);
    std::normal_distribution<double> noise(0.0, 0.01);

    // Ten cluster centres on the 3-axis unit-length grid — corners of
    // a hypercube with axis-aligned positions, so distance between any
    // two distinct clusters is at least 1.0. Within-cluster spread
    // stays well under 0.5, so the true top-k never crosses a cluster
    // boundary for k <= kPerCluster.
    std::vector<std::vector<double>> centres = {
        {0.0, 0.0, 0.0},
        {0.0, 0.0, 3.0},
        {0.0, 3.0, 0.0},
        {0.0, 3.0, 3.0},
        {3.0, 0.0, 0.0},
        {3.0, 0.0, 3.0},
        {3.0, 3.0, 0.0},
        {3.0, 3.0, 3.0},
        {6.0, 0.0, 0.0},
        {0.0, 6.0, 0.0},
    };

    std::vector<std::pair<std::vector<double>, std::string>> dataset;
    dataset.reserve(kDatasetSize);
    for (size_t c = 0; c < kClusters; ++c) {
        for (size_t i = 0; i < kPerCluster; ++i) {
            std::vector<double> v(kDim);
            for (size_t d = 0; d < kDim; ++d) {
                v[d] = centres[c][d] + noise(rng);
            }
            std::string key = "doc_" + std::to_string(c * kPerCluster + i);
            dataset.emplace_back(std::move(v), key);
        }
    }
    ASSERT_EQ(kDatasetSize, dataset.size());

    // Build HNSW index (M=16, ef_construction=200, dim=3, L2 metric).
    ql::hnsw_graph_t hnsw(16, 200, kDim, ql::hnsw_graph_t::metric_t::L2);
    for (const auto &entry : dataset) {
        store_key_t key(std::string(entry.second));
        hnsw.insert(entry.first.data(), key);
    }
    ASSERT_EQ(kDatasetSize, hnsw.size());

    // Generate queries: each one is a cluster centre plus a small
    // noise term, drawn deterministically from the same RNG. The
    // true top-k for each query lives entirely inside that cluster
    // (which has kPerCluster = 10 members, more than k=5).
    std::vector<std::vector<double>> queries;
    queries.reserve(kQueries);
    for (size_t q = 0; q < kQueries; ++q) {
        std::vector<double> qv(kDim);
        for (size_t d = 0; d < kDim; ++d) {
            qv[d] = centres[q % kClusters][d] + noise(rng);
        }
        queries.push_back(std::move(qv));
    }

    double total_recall = 0.0;
    for (const auto &query : queries) {
        auto exact = brute_force_knn(dataset, query, k);
        ASSERT_EQ(k, exact.size());
        auto approx = hnsw.search_knn(query.data(), static_cast<int>(k),
                                     kEfSearch);
        ASSERT_EQ(k, approx.size());
        total_recall += recall_at_k(approx, exact);
    }
    double avg_recall = total_recall / static_cast<double>(kQueries);
    EXPECT_GE(avg_recall, kMinRecall)
        << "HNSW recall " << avg_recall << " below " << kMinRecall
        << " on a clustered 100-vector dataset (k=" << k
        << ", ef_search=" << kEfSearch << ").";
}

// ── IVFFlat recall ───────────────────────────────────────────────────

TEST(VectorCorrectnessTest, IVFFlatRecallOnSmallDataset) {
    // IVFFlat recall test: same clustered 100-vector 3-D dataset as
    // the HNSW test. Train IVFFlat on the dataset, insert, then
    // measure recall@5. With nlist=10 and nprobe=3, expect recall
    // >= 90 percent on a well-separated clustered dataset.

    constexpr size_t kDatasetSize = 100;
    constexpr size_t kClusters = 10;
    constexpr size_t kPerCluster = kDatasetSize / kClusters;
    constexpr size_t kQueries = 20;
    constexpr size_t k = 5;
    constexpr double kMinRecall = 0.90;

    std::mt19937 rng(0xC0FFEEu);
    std::normal_distribution<double> noise(0.0, 0.01);

    std::vector<std::vector<double>> centres = {
        {0.0, 0.0, 0.0}, {0.0, 0.0, 3.0}, {0.0, 3.0, 0.0},
        {0.0, 3.0, 3.0}, {3.0, 0.0, 0.0}, {3.0, 0.0, 3.0},
        {3.0, 3.0, 0.0}, {3.0, 3.0, 3.0}, {6.0, 0.0, 0.0},
        {0.0, 6.0, 0.0},
    };

    std::vector<std::pair<std::vector<double>, std::string>> dataset;
    dataset.reserve(kDatasetSize);
    for (size_t c = 0; c < kClusters; ++c) {
        for (size_t i = 0; i < kPerCluster; ++i) {
            std::vector<double> v(kDim);
            for (size_t d = 0; d < kDim; ++d) v[d] = centres[c][d] + noise(rng);
            dataset.emplace_back(std::move(v),
                "doc_" + std::to_string(c * kPerCluster + i));
        }
    }

    // Train and build IVFFlat index.
    ql::ivfflat_index_t ivf(kClusters, 3, kDim,
                            ql::ivfflat_index_t::metric_t::L2);
    std::vector<std::vector<double>> training;
    for (const auto &entry : dataset) training.push_back(entry.first);
    ivf.train(training, 20);
    ASSERT_TRUE(ivf.is_trained());

    for (const auto &entry : dataset) {
        store_key_t key(std::string(entry.second));
        ivf.insert(entry.first.data(), key);
    }
    ASSERT_EQ(kDatasetSize, ivf.size());

    // Generate queries.
    std::vector<std::vector<double>> queries;
    queries.reserve(kQueries);
    for (size_t q = 0; q < kQueries; ++q) {
        std::vector<double> qv(kDim);
        for (size_t d = 0; d < kDim; ++d) qv[d] = centres[q % kClusters][d] + noise(rng);
        queries.push_back(std::move(qv));
    }

    double total_recall = 0.0;
    for (const auto &query : queries) {
        auto exact = brute_force_knn(dataset, query, k);
        ASSERT_EQ(k, exact.size());
        auto approx = ivf.search_knn(query.data(), static_cast<int>(k), 3);
        ASSERT_GE(approx.size(), 1);
        total_recall += recall_at_k(approx, exact);
    }
    double avg_recall = total_recall / static_cast<double>(kQueries);
    EXPECT_GE(avg_recall, kMinRecall)
        << "IVFFlat recall " << avg_recall << " below " << kMinRecall
        << " (k=" << k << ", nlist=" << kClusters << ", nprobe=3).";
}

// ── Exact match ───────────────────────────────────────────────────────

TEST(VectorCorrectnessTest, HNSWExactMatchOnTinyDataset) {
    // With a small dataset and high ef_search, k=1 HNSW result must
    // match the exact brute-force nearest neighbor for every vector.
    constexpr size_t kDatasetSize = 10;
    constexpr size_t kDim = 4;
    constexpr int kEfSearch = 500;

    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(-10.0, 10.0);

    std::vector<std::pair<std::vector<double>, std::string>> dataset;
    for (size_t i = 0; i < kDatasetSize; ++i) {
        std::vector<double> v(kDim);
        for (size_t d = 0; d < kDim; ++d) v[d] = dist(rng);
        dataset.emplace_back(std::move(v), "doc_" + std::to_string(i));
    }

    ql::hnsw_graph_t hnsw(16, 200, kDim, ql::hnsw_graph_t::metric_t::L2);
    for (const auto &entry : dataset) {
        store_key_t key(std::string(entry.second));
        hnsw.insert(entry.first.data(), key);
    }

    // For each dataset vector, query with itself — the exact nearest
    // neighbor is itself (distance 0), and HNSW must find it.
    for (const auto &entry : dataset) {
        auto exact = brute_force_knn(dataset, entry.first, 1);
        ASSERT_EQ(1u, exact.size());
        auto approx = hnsw.search_knn(entry.first.data(), 1, kEfSearch);
        ASSERT_EQ(1u, approx.size());

        std::string approx_key(
            reinterpret_cast<const char *>(approx[0].second.contents()),
            approx[0].second.size());
        EXPECT_EQ(exact[0].second, approx_key)
            << "HNSW exact match failed for " << entry.second;
    }
}

// ── Performance benchmarks ────────────────────────────────────────────

TEST(VectorCorrectnessTest, BENCHMARK_DistanceFunctions) {
    constexpr size_t kDimB = 128;
    constexpr size_t kIters = 10000;

    std::mt19937 rng(123);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    std::vector<double> a(kDimB), b(kDimB);
    for (size_t i = 0; i < kDimB; ++i) {
        a[i] = dist(rng);
        b[i] = dist(rng);
    }

    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < kIters; ++i)
        ql::l2_distance(a.data(), b.data(), kDimB);
    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(end - start).count();

    std::cout << "BENCH: L2-128 x " << kIters << ": " << elapsed_ms
              << " ms\n";
    // Verify result is sensible (non-negative distance).
    double d = ql::l2_distance(a.data(), b.data(), kDimB);
    EXPECT_GE(d, 0.0);
}

TEST(VectorCorrectnessTest, BENCHMARK_HNSWInsertAndSearch) {
    constexpr size_t kDimB = 32;
    constexpr size_t kDataset = 1000;
    constexpr size_t kQueries = 100;
    constexpr int kK = 10;
    constexpr int kEfSearch = 100;

    std::mt19937 rng(456);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    std::vector<std::pair<std::vector<double>, std::string>> dataset;
    for (size_t i = 0; i < kDataset; ++i) {
        std::vector<double> v(kDimB);
        for (size_t d = 0; d < kDimB; ++d) v[d] = dist(rng);
        dataset.emplace_back(std::move(v), "doc_" + std::to_string(i));
    }

    ql::hnsw_graph_t hnsw(16, 200, kDimB, ql::hnsw_graph_t::metric_t::L2);

    auto start = std::chrono::high_resolution_clock::now();
    for (const auto &entry : dataset) {
        store_key_t key(std::string(entry.second));
        hnsw.insert(entry.first.data(), key);
    }
    auto mid = std::chrono::high_resolution_clock::now();

    std::vector<std::vector<double>> queries;
    for (size_t i = 0; i < kQueries; ++i) {
        std::vector<double> q(kDimB);
        for (size_t d = 0; d < kDimB; ++d) q[d] = dist(rng);
        queries.push_back(std::move(q));
    }

    for (const auto &q : queries)
        hnsw.search_knn(q.data(), kK, kEfSearch);

    auto end = std::chrono::high_resolution_clock::now();
    auto insert_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(mid - start).count();
    auto search_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(end - mid).count();

    std::cout << "BENCH: HNSW insert 1000x32-dim: " << insert_ms << " ms\n";
    std::cout << "BENCH: HNSW search 100x " << kK << "-NN: " << search_ms
              << " ms\n";

    ASSERT_EQ(kDataset, hnsw.size());
}

TEST(VectorCorrectnessTest, BENCHMARK_IVFFlatInsertAndSearch) {
    constexpr size_t kDimB = 32;
    constexpr size_t kDataset = 1000;
    constexpr size_t kQueries = 100;
    constexpr int kK = 10;
    constexpr int kNlist = 20;
    constexpr int kNprobe = 5;

    std::mt19937 rng(789);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    std::vector<std::pair<std::vector<double>, std::string>> dataset;
    for (size_t i = 0; i < kDataset; ++i) {
        std::vector<double> v(kDimB);
        for (size_t d = 0; d < kDimB; ++d) v[d] = dist(rng);
        dataset.emplace_back(std::move(v), "doc_" + std::to_string(i));
    }

    ql::ivfflat_index_t ivf(kNlist, kNprobe, kDimB,
                            ql::ivfflat_index_t::metric_t::L2);

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::vector<double>> training;
    for (const auto &entry : dataset) training.push_back(entry.first);
    ivf.train(training, 10);

    for (const auto &entry : dataset) {
        store_key_t key(std::string(entry.second));
        ivf.insert(entry.first.data(), key);
    }
    auto mid = std::chrono::high_resolution_clock::now();

    std::vector<std::vector<double>> queries;
    for (size_t i = 0; i < kQueries; ++i) {
        std::vector<double> q(kDimB);
        for (size_t d = 0; d < kDimB; ++d) q[d] = dist(rng);
        queries.push_back(std::move(q));
    }

    for (const auto &q : queries)
        ivf.search_knn(q.data(), kK, kNprobe);

    auto end = std::chrono::high_resolution_clock::now();
    auto train_insert_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(mid - start).count();
    auto search_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(end - mid).count();

    std::cout << "BENCH: IVFFlat train+insert 1000x32-dim: "
              << train_insert_ms << " ms\n";
    std::cout << "BENCH: IVFFlat search 100x " << kK << "-NN: "
              << search_ms << " ms\n";

    ASSERT_TRUE(ivf.is_trained());
    ASSERT_EQ(kDataset, ivf.size());
}

// ── Stress tests ──────────────────────────────────────────────────────

TEST(VectorCorrectnessTest, HNSWStress_10000Vectors) {
    constexpr size_t kDimS = 8;
    constexpr size_t kDataset = 10000;
    constexpr size_t kQueries = 50;
    constexpr int kK = 5;
    constexpr int kEfSearch = 100;

    std::mt19937 rng(999);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    std::vector<std::pair<std::vector<double>, std::string>> dataset;
    for (size_t i = 0; i < kDataset; ++i) {
        std::vector<double> v(kDimS);
        for (size_t d = 0; d < kDimS; ++d) v[d] = dist(rng);
        dataset.emplace_back(std::move(v), "doc_" + std::to_string(i));
    }

    ql::hnsw_graph_t hnsw(16, 200, kDimS, ql::hnsw_graph_t::metric_t::L2);
    for (const auto &entry : dataset) {
        store_key_t key(std::string(entry.second));
        hnsw.insert(entry.first.data(), key);
    }
    ASSERT_EQ(kDataset, hnsw.size());

    for (size_t i = 0; i < kQueries; ++i) {
        std::vector<double> q(kDimS);
        for (size_t d = 0; d < kDimS; ++d) q[d] = dist(rng);
        auto results = hnsw.search_knn(q.data(), kK, kEfSearch);
        ASSERT_FALSE(results.empty());
        EXPECT_LT(results[0].first,
                  std::numeric_limits<double>::max());
    }
}

TEST(VectorCorrectnessTest, IVFFlatStress_10000Vectors) {
    constexpr size_t kDimS = 8;
    constexpr size_t kDataset = 10000;
    constexpr size_t kQueries = 50;
    constexpr int kK = 5;
    constexpr int kNlist = 50;
    constexpr int kNprobe = 5;

    std::mt19937 rng(111);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    std::vector<std::pair<std::vector<double>, std::string>> dataset;
    for (size_t i = 0; i < kDataset; ++i) {
        std::vector<double> v(kDimS);
        for (size_t d = 0; d < kDimS; ++d) v[d] = dist(rng);
        dataset.emplace_back(std::move(v), "doc_" + std::to_string(i));
    }

    ql::ivfflat_index_t ivf(kNlist, kNprobe, kDimS,
                            ql::ivfflat_index_t::metric_t::L2);

    std::vector<std::vector<double>> training;
    for (const auto &entry : dataset) training.push_back(entry.first);
    ivf.train(training, 10);
    ASSERT_TRUE(ivf.is_trained());

    for (const auto &entry : dataset) {
        store_key_t key(std::string(entry.second));
        ivf.insert(entry.first.data(), key);
    }
    ASSERT_EQ(kDataset, ivf.size());

    for (size_t i = 0; i < kQueries; ++i) {
        std::vector<double> q(kDimS);
        for (size_t d = 0; d < kDimS; ++d) q[d] = dist(rng);
        auto results = ivf.search_knn(q.data(), kK, kNprobe);
        ASSERT_FALSE(results.empty());
        EXPECT_LT(results[0].first,
                  std::numeric_limits<double>::max());
    }
}

}  // namespace unittest
