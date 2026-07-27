// Copyright 2026 RethinkDB, all rights reserved.
//
// PERF-BENCH: Vector index operations (L2 search, HNSW build) benchmarks.
//
// Measures:
//   - L2 distance computation latency (micro-benchmark, various dims)
//   - HNSW graph build time for different dataset sizes
//   - HNSW single-query KNN search latency
//   - HNSW batch-query KNN search throughput
//
// Uses std::chrono for timing via GTest harness — no Google Benchmark dep.

#include "unittest/gtest.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "btree/keys.hpp"
#include "rdb_protocol/hnsw.hpp"
#include "rdb_protocol/vector_distance.hpp"

namespace unittest {

namespace {

// ── Benchmark helpers ─────────────────────────────────────────────────

struct bench_stats_t {
    double min_us;
    double mean_us;
    double median_us;
    double max_us;
    size_t iterations;

    void print(const std::string &label) const {
        std::cout << "BENCH: " << label << "\n"
                  << "  iters=" << iterations
                  << "  min=" << std::fixed << std::setprecision(1) << min_us
                  << "us  mean=" << mean_us
                  << "us  median=" << median_us
                  << "us  max=" << max_us << "us\n";
    }
};

bench_stats_t compute_stats(std::vector<double> &durations_us,
                            size_t iterations) {
    std::sort(durations_us.begin(), durations_us.end());
    double sum = std::accumulate(durations_us.begin(), durations_us.end(), 0.0);
    return {
        durations_us.front(),
        sum / static_cast<double>(iterations),
        durations_us[iterations / 2],
        durations_us.back(),
        iterations,
    };
}

// ── Vector data generation ────────────────────────────────────────────

std::pair<std::vector<double>, std::vector<double>> random_vector_pair(
    size_t dim, std::mt19937 &rng) {
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    std::vector<double> a(dim), b(dim);
    for (size_t d = 0; d < dim; ++d) {
        a[d] = dist(rng);
        b[d] = dist(rng);
    }
    return {std::move(a), std::move(b)};
}

void insert_dataset(ql::hnsw_graph_t &hnsw,
                    const std::vector<std::pair<std::vector<double>,
                                                std::string>> &dataset) {
    for (const auto &entry : dataset) {
        store_key_t key(std::string(entry.second));
        hnsw.insert(entry.first.data(), key);
    }
}

}  // namespace

// ── L2 Distance Microbenchmarks ───────────────────────────────────────

TEST(BenchmarkVector, L2Distance_3D) {
    constexpr size_t kIters = 50000;
    constexpr size_t kDim = 3;

    std::mt19937 rng(42);
    auto [a, b] = random_vector_pair(kDim, rng);

    // Warmup
    for (size_t i = 0; i < 1000; ++i) ql::l2_distance(a.data(), b.data(), kDim);

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        volatile double d = ql::l2_distance(a.data(), b.data(), kDim);
        (void)d;
        auto end = std::chrono::high_resolution_clock::now();
        durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("Vector: L2 distance 3-D");
}

TEST(BenchmarkVector, L2Distance_128D) {
    constexpr size_t kIters = 10000;
    constexpr size_t kDim = 128;

    std::mt19937 rng(42);
    auto [a, b] = random_vector_pair(kDim, rng);

    for (size_t i = 0; i < 500; ++i) ql::l2_distance(a.data(), b.data(), kDim);

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        volatile double d = ql::l2_distance(a.data(), b.data(), kDim);
        (void)d;
        auto end = std::chrono::high_resolution_clock::now();
        durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("Vector: L2 distance 128-D");
}

TEST(BenchmarkVector, L2Distance_768D) {
    constexpr size_t kIters = 2000;
    constexpr size_t kDim = 768;

    std::mt19937 rng(42);
    auto [a, b] = random_vector_pair(kDim, rng);

    for (size_t i = 0; i < 100; ++i) ql::l2_distance(a.data(), b.data(), kDim);

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        volatile double d = ql::l2_distance(a.data(), b.data(), kDim);
        (void)d;
        auto end = std::chrono::high_resolution_clock::now();
        durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("Vector: L2 distance 768-D");
}

// ── Cosine Distance Microbenchmarks ───────────────────────────────────

TEST(BenchmarkVector, CosineDistance_128D) {
    constexpr size_t kIters = 5000;
    constexpr size_t kDim = 128;

    std::mt19937 rng(42);
    auto [a, b] = random_vector_pair(kDim, rng);

    for (size_t i = 0; i < 200; ++i)
        ql::cosine_distance(a.data(), b.data(), kDim);

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        volatile double d = ql::cosine_distance(a.data(), b.data(), kDim);
        (void)d;
        auto end = std::chrono::high_resolution_clock::now();
        durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("Vector: cosine distance 128-D");
}

// ── Inner Product Distance Microbenchmark ─────────────────────────────

TEST(BenchmarkVector, InnerProductDistance_128D) {
    constexpr size_t kIters = 5000;
    constexpr size_t kDim = 128;

    std::mt19937 rng(42);
    auto [a, b] = random_vector_pair(kDim, rng);

    for (size_t i = 0; i < 200; ++i)
        ql::inner_product_distance(a.data(), b.data(), kDim);

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        volatile double d =
            ql::inner_product_distance(a.data(), b.data(), kDim);
        (void)d;
        auto end = std::chrono::high_resolution_clock::now();
        durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("Vector: inner-product distance 128-D");
}

// ── HNSW Graph Build ──────────────────────────────────────────────

TEST(BenchmarkVector, HNSW_Build_1000x32D) {
    constexpr size_t kDim = 32;
    constexpr size_t kDataset = 1000;
    constexpr size_t kIters = 5;  // build is expensive, fewer iters

    std::mt19937 rng(789);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    // Pre-generate dataset
    std::vector<std::pair<std::vector<double>, std::string>> dataset;
    for (size_t i = 0; i < kDataset; ++i) {
        std::vector<double> v(kDim);
        for (size_t d = 0; d < kDim; ++d) v[d] = dist(rng);
        dataset.emplace_back(std::move(v), "doc_" + std::to_string(i));
    }

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t iter = 0; iter < kIters; ++iter) {
        ql::hnsw_graph_t hnsw(16, 200, kDim, ql::hnsw_graph_t::metric_t::L2);

        auto start = std::chrono::high_resolution_clock::now();
        insert_dataset(hnsw, dataset);
        auto end = std::chrono::high_resolution_clock::now();

        double ms = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                end - start).count()) / 1000.0;
        durations.push_back(ms * 1000.0);  // store in us

        ASSERT_EQ(kDataset, hnsw.size());
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("Vector: HNSW build 1000 x 32-D");

    double mean_ms = stats.mean_us / 1000.0;
    std::cout << "  mean build time=" << std::fixed << std::setprecision(2)
              << mean_ms << " ms\n";
}

TEST(BenchmarkVector, HNSW_Build_5000x16D) {
    constexpr size_t kDim = 16;
    constexpr size_t kDataset = 5000;
    constexpr size_t kIters = 3;

    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    std::vector<std::pair<std::vector<double>, std::string>> dataset;
    for (size_t i = 0; i < kDataset; ++i) {
        std::vector<double> v(kDim);
        for (size_t d = 0; d < kDim; ++d) v[d] = dist(rng);
        dataset.emplace_back(std::move(v), "doc_" + std::to_string(i));
    }

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t iter = 0; iter < kIters; ++iter) {
        ql::hnsw_graph_t hnsw(16, 200, kDim, ql::hnsw_graph_t::metric_t::L2);

        auto start = std::chrono::high_resolution_clock::now();
        insert_dataset(hnsw, dataset);
        auto end = std::chrono::high_resolution_clock::now();

        double ms = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                end - start).count()) / 1000.0;
        durations.push_back(ms * 1000.0);  // store in us

        ASSERT_EQ(kDataset, hnsw.size());
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("Vector: HNSW build 5000 x 16-D");

    double mean_ms = stats.mean_us / 1000.0;
    std::cout << "  mean build time=" << std::fixed << std::setprecision(2)
              << mean_ms << " ms\n";
}

// ── HNSW Single Query KNN Search ──────────────────────────────────

TEST(BenchmarkVector, HNSW_SingleQuery_1000x32D) {
    constexpr size_t kDim = 32;
    constexpr size_t kDataset = 1000;
    constexpr size_t kQueries = 500;
    constexpr int kK = 10;
    constexpr int kEfSearch = 100;

    std::mt19937 rng(456);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    // Build index once
    std::vector<std::pair<std::vector<double>, std::string>> dataset;
    for (size_t i = 0; i < kDataset; ++i) {
        std::vector<double> v(kDim);
        for (size_t d = 0; d < kDim; ++d) v[d] = dist(rng);
        dataset.emplace_back(std::move(v), "doc_" + std::to_string(i));
    }

    ql::hnsw_graph_t hnsw(16, 200, kDim, ql::hnsw_graph_t::metric_t::L2);
    insert_dataset(hnsw, dataset);
    ASSERT_EQ(kDataset, hnsw.size());

    // Generate queries
    std::vector<std::vector<double>> queries;
    for (size_t i = 0; i < kQueries; ++i) {
        std::vector<double> q(kDim);
        for (size_t d = 0; d < kDim; ++d) q[d] = dist(rng);
        queries.push_back(std::move(q));
    }

    // Warmup
    for (size_t i = 0; i < 20; ++i)
        hnsw.search_knn(queries[i % kQueries].data(), kK, kEfSearch);

    std::vector<double> durations;
    durations.reserve(kQueries);

    for (size_t i = 0; i < kQueries; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        auto results = hnsw.search_knn(queries[i].data(), kK, kEfSearch);
        auto end = std::chrono::high_resolution_clock::now();
        durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);
        EXPECT_EQ(static_cast<size_t>(kK), results.size());
    }

    auto stats = compute_stats(durations, kQueries);
    stats.print("Vector: HNSW single-query KNN (k=10, ef=100, 1000x32-D)");
}

TEST(BenchmarkVector, HNSW_SingleQuery_5000x16D) {
    constexpr size_t kDim = 16;
    constexpr size_t kDataset = 5000;
    constexpr size_t kQueries = 200;
    constexpr int kK = 10;
    constexpr int kEfSearch = 100;

    std::mt19937 rng(7890);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    std::vector<std::pair<std::vector<double>, std::string>> dataset;
    for (size_t i = 0; i < kDataset; ++i) {
        std::vector<double> v(kDim);
        for (size_t d = 0; d < kDim; ++d) v[d] = dist(rng);
        dataset.emplace_back(std::move(v), "doc_" + std::to_string(i));
    }

    ql::hnsw_graph_t hnsw(16, 200, kDim, ql::hnsw_graph_t::metric_t::L2);
    insert_dataset(hnsw, dataset);
    ASSERT_EQ(kDataset, hnsw.size());

    std::vector<std::vector<double>> queries;
    for (size_t i = 0; i < kQueries; ++i) {
        std::vector<double> q(kDim);
        for (size_t d = 0; d < kDim; ++d) q[d] = dist(rng);
        queries.push_back(std::move(q));
    }

    for (size_t i = 0; i < 10; ++i)
        hnsw.search_knn(queries[i % kQueries].data(), kK, kEfSearch);

    std::vector<double> durations;
    durations.reserve(kQueries);

    for (size_t i = 0; i < kQueries; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        auto results = hnsw.search_knn(queries[i].data(), kK, kEfSearch);
        auto end = std::chrono::high_resolution_clock::now();
        durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);
        EXPECT_EQ(static_cast<size_t>(kK), results.size());
    }

    auto stats = compute_stats(durations, kQueries);
    stats.print("Vector: HNSW single-query KNN (k=10, ef=100, 5000x16-D)");
}

// ── HNSW Batch Query KNN Search ───────────────────────────────────

TEST(BenchmarkVector, HNSW_BatchQuery_100Queries) {
    constexpr size_t kDim = 32;
    constexpr size_t kDataset = 1000;
    constexpr size_t kBatches = 50;
    constexpr size_t kBatchSize = 100;
    constexpr int kK = 10;
    constexpr int kEfSearch = 100;

    std::mt19937 rng(5678);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    std::vector<std::pair<std::vector<double>, std::string>> dataset;
    for (size_t i = 0; i < kDataset; ++i) {
        std::vector<double> v(kDim);
        for (size_t d = 0; d < kDim; ++d) v[d] = dist(rng);
        dataset.emplace_back(std::move(v), "doc_" + std::to_string(i));
    }

    ql::hnsw_graph_t hnsw(16, 200, kDim, ql::hnsw_graph_t::metric_t::L2);
    insert_dataset(hnsw, dataset);

    // Generate batch queries
    std::vector<std::vector<double>> query_pool;
    for (size_t i = 0; i < kBatchSize; ++i) {
        std::vector<double> q(kDim);
        for (size_t d = 0; d < kDim; ++d) q[d] = dist(rng);
        query_pool.push_back(std::move(q));
    }

    // Warmup
    for (size_t i = 0; i < 5; ++i) {
        for (const auto &q : query_pool)
            hnsw.search_knn(q.data(), kK, kEfSearch);
    }

    std::vector<double> durations;
    durations.reserve(kBatches);

    for (size_t b = 0; b < kBatches; ++b) {
        auto start = std::chrono::high_resolution_clock::now();
        size_t total_results = 0;
        for (const auto &q : query_pool) {
            auto results = hnsw.search_knn(q.data(), kK, kEfSearch);
            total_results += results.size();
        }
        auto end = std::chrono::high_resolution_clock::now();
        double ms = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                end - start).count()) / 1000.0;
        durations.push_back(ms * 1000.0);  // us
        EXPECT_EQ(kBatchSize * static_cast<size_t>(kK), total_results);
    }

    auto stats = compute_stats(durations, kBatches);
    stats.print("Vector: HNSW batch 100 queries (k=10, ef=100, 1000x32-D)");

    double qps = (static_cast<double>(kBatchSize) / stats.mean_us) * 1e6;
    std::cout << "  throughput=" << std::fixed << std::setprecision(0)
              << qps << " queries/sec\n";
}

}  // namespace unittest
