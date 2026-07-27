// Copyright 2026 RethinkDB, all rights reserved.
//
// PERF-BENCH: BRIN (Block Range Index) between query latency benchmarks.
//
// Measures:
//   - BRIN summary creation throughput
//   - BRIN index build time for varying dataset sizes
//   - BRIN between-query simulation (range pruning via summaries)
//   - BRIN summary validation throughput
//   - Serialization round-trip cost
//
// Uses std::chrono for timing via GTest harness — no Google Benchmark dep.

#include "unittest/gtest.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include "btree/keys.hpp"
#include "containers/archive/stl_types.hpp"
#include "containers/archive/vector_stream.hpp"
#include "rdb_protocol/brin.hpp"
#include "rdb_protocol/datum.hpp"

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

// ── BRIN fixture utilities ────────────────────────────────────────────

brin_index_t make_empty_index(uint64_t range_size = 128) {
    brin_index_t idx;
    idx.format_version = BRIN_FORMAT_VERSION;
    idx.range_size = range_size;
    idx.columns.push_back("value");
    return idx;
}

brin_summary_t make_summary(const std::string &left,
                            const std::string &right,
                            double min_val,
                            double max_val,
                            uint64_t live = 10,
                            uint64_t nulls = 0) {
    brin_summary_t s;
    s.primary_key_left = store_key_t(left);
    s.primary_key_right = key_range_t::right_bound_t(store_key_t(right));
    s.minimum.push_back(ql::datum_t(min_val));
    s.maximum.push_back(ql::datum_t(max_val));
    s.live_row_count = live;
    s.null_row_count = nulls;
    s.dirty = false;
    return s;
}

// Mirror the production build loop from protocol.cc:565-605
brin_index_t simulate_brin_build(
    std::vector<std::pair<store_key_t, ql::datum_t>> entries,
    uint64_t range_size) {
    std::sort(entries.begin(), entries.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });

    brin_index_t idx;
    idx.format_version = BRIN_FORMAT_VERSION;
    idx.range_size = range_size;
    idx.columns.push_back("value");

    const size_t n = entries.size();
    size_t range_start = 0;
    while (range_start < n) {
        const size_t range_end = std::min(range_start + range_size, n);
        brin_summary_t summary;
        summary.primary_key_left = entries[range_start].first;
        if (range_end < n) {
            summary.primary_key_right =
                key_range_t::right_bound_t(entries[range_end].first);
        } else {
            summary.primary_key_right =
                key_range_t::right_bound_t::make_unbounded();
        }

        ql::datum_t col_min = entries[range_start].second;
        ql::datum_t col_max = entries[range_start].second;
        summary.live_row_count = 0;
        summary.null_row_count = 0;

        for (size_t i = range_start; i < range_end; ++i) {
            const ql::datum_t &val = entries[i].second;
            if (!val.has()) {
                ++summary.null_row_count;
                continue;
            }
            ++summary.live_row_count;
            if (val.cmp(col_min) < 0) col_min = val;
            if (col_max.cmp(val) < 0) col_max = val;
        }

        summary.minimum.push_back(col_min);
        summary.maximum.push_back(col_max);
        summary.dirty = false;
        idx.summaries.push_back(std::move(summary));
        range_start = range_end;
    }
    return idx;
}

// Generate deterministic entries
std::vector<std::pair<store_key_t, ql::datum_t>> generate_entries(
    size_t count, uint64_t seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> dist(0.0, 1000.0);

    std::vector<std::pair<store_key_t, ql::datum_t>> entries;
    entries.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        char key_buf[32];
        snprintf(key_buf, sizeof(key_buf), "key_%08zu", i);
        entries.emplace_back(store_key_t(std::string(key_buf)),
                             ql::datum_t(dist(rng)));
    }
    return entries;
}

}  // namespace

// ── BRIN Summary Creation ─────────────────────────────────────────────

TEST(BenchmarkBRIN, SummaryCreation) {
    constexpr size_t kIters = 20000;
    constexpr size_t kWarmup = 1000;

    for (size_t i = 0; i < kWarmup; ++i) {
        make_summary("aaa", "zzz", 1.0, 100.0);
    }

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        auto s = make_summary("aaa", "zzz", 1.0, 100.0);
        auto end = std::chrono::high_resolution_clock::now();
        durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);
        EXPECT_EQ(1u, s.minimum.size());
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("BRIN: summary creation");
}

// ── BRIN Index Build ──────────────────────────────────────────────────

TEST(BenchmarkBRIN, IndexBuild_1000Entries) {
    constexpr size_t kEntries = 1000;
    constexpr uint64_t kRangeSize = 128;
    constexpr size_t kIters = 20;

    auto entries = generate_entries(kEntries);

    // Warmup
    for (size_t i = 0; i < 3; ++i)
        simulate_brin_build(entries, kRangeSize);

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        // Clone entries to avoid move side-effects
        auto entries_copy = entries;

        auto start = std::chrono::high_resolution_clock::now();
        auto idx = simulate_brin_build(entries_copy, kRangeSize);
        auto end = std::chrono::high_resolution_clock::now();

        double us = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0;
        durations.push_back(us);

        // Validate the result
        std::string err;
        EXPECT_TRUE(validate_brin_index(idx, &err)) << err;
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("BRIN: index build 1000 entries (range_size=128)");

    double entries_per_sec = (static_cast<double>(kEntries) / stats.mean_us) * 1e6;
    std::cout << "  throughput=" << std::fixed << std::setprecision(0)
              << entries_per_sec << " entries/sec\n";
}

TEST(BenchmarkBRIN, IndexBuild_10000Entries) {
    constexpr size_t kEntries = 10000;
    constexpr uint64_t kRangeSize = 128;
    constexpr size_t kIters = 10;

    auto entries = generate_entries(kEntries, 99);

    for (size_t i = 0; i < 3; ++i)
        simulate_brin_build(entries, kRangeSize);

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        auto entries_copy = entries;

        auto start = std::chrono::high_resolution_clock::now();
        auto idx = simulate_brin_build(entries_copy, kRangeSize);
        auto end = std::chrono::high_resolution_clock::now();

        double us = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0;
        durations.push_back(us);

        std::string err;
        EXPECT_TRUE(validate_brin_index(idx, &err)) << err;
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("BRIN: index build 10000 entries (range_size=128)");

    double entries_per_sec = (static_cast<double>(kEntries) / stats.mean_us) * 1e6;
    std::cout << "  throughput=" << std::fixed << std::setprecision(0)
              << entries_per_sec << " entries/sec\n";
}

TEST(BenchmarkBRIN, IndexBuild_VaryingRangeSize) {
    constexpr size_t kEntries = 5000;
    constexpr size_t kIters = 5;

    auto entries = generate_entries(kEntries, 777);

    const uint64_t range_sizes[] = {16, 64, 128, 512, 1024};

    std::cout << "BENCH: BRIN index build by range_size (5000 entries)\n";
    for (auto rs : range_sizes) {
        // Warmup
        simulate_brin_build(entries, rs);

        std::vector<double> durations;
        durations.reserve(kIters);

        for (size_t i = 0; i < kIters; ++i) {
            auto entries_copy = entries;
            auto start = std::chrono::high_resolution_clock::now();
            auto idx = simulate_brin_build(std::move(entries_copy), rs);
            auto end = std::chrono::high_resolution_clock::now();
            double us = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    end - start).count()) / 1000.0;
            durations.push_back(us);
            std::string err;
            EXPECT_TRUE(validate_brin_index(idx, &err)) << err;
        }

        auto stats = compute_stats(durations, kIters);
        std::cout << "  range_size=" << std::setw(4) << rs
                  << "  summaries=" << std::setw(4)
                  << ((kEntries + rs - 1) / rs)
                  << "  mean=" << std::fixed << std::setprecision(1)
                  << stats.mean_us << "us\n";
    }
}

// ── BRIN Between Query Simulation ─────────────────────────────────────

TEST(BenchmarkBRIN, BetweenQuerySimulation) {
    // Simulates a BETWEEN query using BRIN summaries: for a given value
    // range [lo, hi], scan the summaries list and count how many ranges
    // intersect — those need a full table scan. This models the pruning
    // decision.
    constexpr size_t kSummaries = 1000;
    constexpr size_t kQueries = 5000;
    constexpr size_t kWarmup = 200;

    // Build a BRIN index with many summaries
    brin_index_t idx = make_empty_index(64);
    for (size_t i = 0; i < kSummaries; ++i) {
        char left[32], right[32];
        snprintf(left, sizeof(left), "r%06zu_a", i);
        snprintf(right, sizeof(right), "r%06zu_z", i);
        double lo = static_cast<double>(i * 10);
        double hi = lo + 20.0;
        idx.summaries.push_back(
            make_summary(left, right, lo, hi, 64));
    }

    // Warmup
    volatile size_t sink = 0;
    for (size_t i = 0; i < kWarmup; ++i) {
        for (const auto &s : idx.summaries) {
            double s_min = s.minimum[0].as_num();
            double s_max = s.maximum[0].as_num();
            if (!(s_max < 50.0 || s_min > 150.0)) ++sink;
        }
    }
    (void)sink;

    std::vector<double> durations;
    durations.reserve(kQueries);

    // Query range: BETWEEN 50 AND 150
    for (size_t q = 0; q < kQueries; ++q) {
        double q_lo = 50.0;
        double q_hi = 150.0;

        auto start = std::chrono::high_resolution_clock::now();
        size_t matching_ranges = 0;
        for (const auto &s : idx.summaries) {
            double s_min = s.minimum[0].as_num();
            double s_max = s.maximum[0].as_num();
            // Range intersects if NOT (s_max < q_lo OR s_min > q_hi)
            if (!(s_max < q_lo || s_min > q_hi)) {
                ++matching_ranges;
            }
        }
        auto end = std::chrono::high_resolution_clock::now();
        durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);
        EXPECT_GT(matching_ranges, 0u);
    }

    auto stats = compute_stats(durations, kQueries);
    stats.print("BRIN: between-query simulation (1000 summaries, BETWEEN 50-150)");

    double qps = (1.0 / stats.mean_us) * 1e6;
    std::cout << "  queries/sec=" << std::fixed << std::setprecision(0)
              << qps << "\n";
}

TEST(BenchmarkBRIN, BetweenQuery_SelectiveRange) {
    // Narrow range that prunes most summaries — models an efficient BRIN scan.
    constexpr size_t kSummaries = 1000;
    constexpr size_t kQueries = 5000;
    constexpr size_t kWarmup = 200;

    brin_index_t idx = make_empty_index(64);
    for (size_t i = 0; i < kSummaries; ++i) {
        char left[32], right[32];
        snprintf(left, sizeof(left), "r%06zu_a", i);
        snprintf(right, sizeof(right), "r%06zu_z", i);
        double lo = static_cast<double>(i * 10);
        double hi = lo + 20.0;
        idx.summaries.push_back(
            make_summary(left, right, lo, hi, 64));
    }

    volatile size_t sink = 0;
    for (size_t i = 0; i < kWarmup; ++i) {
        for (const auto &s : idx.summaries) {
            double s_min = s.minimum[0].as_num();
            double s_max = s.maximum[0].as_num();
            if (!(s_max < 100.0 || s_min > 110.0)) ++sink;
        }
    }
    (void)sink;

    std::vector<double> durations;
    durations.reserve(kQueries);

    // Selective range: BETWEEN 100 AND 110 (touches ~2 summaries out of 1000)
    for (size_t q = 0; q < kQueries; ++q) {
        auto start = std::chrono::high_resolution_clock::now();
        size_t matching_ranges = 0;
        for (const auto &s : idx.summaries) {
            double s_min = s.minimum[0].as_num();
            double s_max = s.maximum[0].as_num();
            if (!(s_max < 100.0 || s_min > 110.0)) {
                ++matching_ranges;
            }
        }
        auto end = std::chrono::high_resolution_clock::now();
        durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);
        EXPECT_LE(matching_ranges, 5u);  // very selective
    }

    auto stats = compute_stats(durations, kQueries);
    stats.print(
        "BRIN: between-query 1000 summaries (selective BETWEEN 100-110)");

    double qps = (1.0 / stats.mean_us) * 1e6;
    std::cout << "  queries/sec=" << std::fixed << std::setprecision(0)
              << qps << "\n";
}

// ── BRIN Validation Throughput ────────────────────────────────────────

TEST(BenchmarkBRIN, ValidationThroughput) {
    constexpr size_t kSummaries = 500;
    constexpr size_t kIters = 2000;
    constexpr size_t kWarmup = 50;

    // Build a valid index
    brin_index_t idx = make_empty_index(32);
    for (size_t i = 0; i < kSummaries; ++i) {
        char left[32], right[32];
        snprintf(left, sizeof(left), "s%06zu_a", i);
        snprintf(right, sizeof(right), "s%06zu_z", i);
        idx.summaries.push_back(
            make_summary(left, right,
                         static_cast<double>(i * 5),
                         static_cast<double>(i * 5 + 10), 32));
    }

    // Warmup
    std::string err;
    for (size_t i = 0; i < kWarmup; ++i)
        validate_brin_index(idx, &err);

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        std::string validate_err;
        auto start = std::chrono::high_resolution_clock::now();
        bool valid = validate_brin_index(idx, &validate_err);
        auto end = std::chrono::high_resolution_clock::now();
        durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);
        EXPECT_TRUE(valid) << validate_err;
    }

    auto stats = compute_stats(durations, kIters);
    stats.print(
        "BRIN: validate index (" + std::to_string(kSummaries) + " summaries)");
}

// ── BRIN Summary Serialization ────────────────────────────────────────

TEST(BenchmarkBRIN, SerializationRoundTrip) {
    constexpr size_t kIters = 5000;
    constexpr size_t kWarmup = 200;

    brin_summary_t s1 = make_summary("aaa_key_start", "zzz_key_end",
                                     1.0, 42.5, 100, 3);

    // Warmup
    for (size_t i = 0; i < kWarmup; ++i) {
        write_message_t wm;
        serialize<cluster_version_t::LATEST_DISK>(&wm, s1);
        vector_stream_t stream;
        int sr = send_write_message(&stream, &wm);
        ASSERT_EQ(0, sr);
        auto data = stream.vector();
        vector_read_stream_t read_stream(std::move(data));
        brin_summary_t s2;
        deserialize<cluster_version_t::LATEST_DISK>(&read_stream, &s2);
    }

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        auto start = std::chrono::high_resolution_clock::now();

        write_message_t wm;
        serialize<cluster_version_t::LATEST_DISK>(&wm, s1);
        vector_stream_t stream;
        ASSERT_EQ(0, send_write_message(&stream, &wm));
        auto data = stream.vector();
        vector_read_stream_t read_stream(std::move(data));
        brin_summary_t s2;
        ASSERT_EQ(archive_result_t::SUCCESS,
                  deserialize<cluster_version_t::LATEST_DISK>(
                      &read_stream, &s2));

        auto end = std::chrono::high_resolution_clock::now();
        durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);

        EXPECT_EQ(s1.primary_key_left, s2.primary_key_left);
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("BRIN: summary serialize + deserialize roundtrip");
}

// ── BRIN Full Index Serialization ─────────────────────────────────────

TEST(BenchmarkBRIN, FullIndexSerialization) {
    constexpr size_t kSummaries = 200;
    constexpr size_t kIters = 200;

    brin_index_t idx1 = make_empty_index(256);
    for (size_t i = 0; i < kSummaries; ++i) {
        char left[32], right[32];
        snprintf(left, sizeof(left), "idx%06zu_a", i);
        snprintf(right, sizeof(right), "idx%06zu_z", i);
        idx1.summaries.push_back(
            make_summary(left, right,
                         static_cast<double>(i * 10),
                         static_cast<double>(i * 10 + 5), 256));
    }

    // Warmup
    for (size_t i = 0; i < 10; ++i) {
        write_message_t wm;
        serialize<cluster_version_t::LATEST_DISK>(&wm, idx1);
        vector_stream_t stream;
        int sr = send_write_message(&stream, &wm);
        ASSERT_EQ(0, sr);
        auto data = stream.vector();
        vector_read_stream_t read_stream(std::move(data));
        brin_index_t idx2;
        deserialize<cluster_version_t::LATEST_DISK>(&read_stream, &idx2);
    }

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        auto start = std::chrono::high_resolution_clock::now();

        write_message_t wm;
        serialize<cluster_version_t::LATEST_DISK>(&wm, idx1);
        vector_stream_t stream;
        ASSERT_EQ(0, send_write_message(&stream, &wm));
        auto data = stream.vector();
        vector_read_stream_t read_stream(std::move(data));
        brin_index_t idx2;
        ASSERT_EQ(archive_result_t::SUCCESS,
                  deserialize<cluster_version_t::LATEST_DISK>(
                      &read_stream, &idx2));

        auto end = std::chrono::high_resolution_clock::now();
        double us = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0;
        durations.push_back(us);

        EXPECT_EQ(idx1.summaries.size(), idx2.summaries.size());
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("BRIN: full index serialize + deserialize (" +
                std::to_string(kSummaries) + " summaries)");
}

}  // namespace unittest
