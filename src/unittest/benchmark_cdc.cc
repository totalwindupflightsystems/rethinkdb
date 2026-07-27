// Copyright 2026 RethinkDB, all rights reserved.
//
// PERF-BENCH: CDC changefeed delivery throughput benchmarks.
//
// Measures:
//   - change_record_t creation throughput (records/sec)
//   - Batch record production and event_id dedup throughput
//   - Small-batch delivery simulation (events/sec over virtual changefeed)
//
// Uses std::chrono for timing via GTest harness — no Google Benchmark dep.

#include "unittest/gtest.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "containers/uuid.hpp"
#include "rdb_protocol/cdc_types.hpp"

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

// ── Fixture utilities ─────────────────────────────────────────────────

std::vector<uuid_u> make_shard_ids(size_t count) {
    std::vector<uuid_u> ids;
    ids.reserve(count);
    for (size_t i = 0; i < count; ++i) ids.push_back(generate_uuid());
    return ids;
}

std::vector<ql::change_record_t> make_cdc_batch(
    const uuid_u &cluster, const uuid_u &table,
    const std::vector<uuid_u> &shard_ids,
    uint64_t start_lsn, size_t count) {

    std::vector<ql::change_record_t> batch;
    batch.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        ql::change_record_t r;
        r.event_id = ql::change_event_id_t{
            cluster, table, shard_ids[i % shard_ids.size()],
            {start_lsn + i}};
        r.op = ql::change_operation_t::INSERT;
        r.after_image = {'x'};
        r.commit_timestamp = static_cast<microtime_t>((start_lsn + i) * 1000);
        batch.push_back(std::move(r));
    }
    return batch;
}

}  // namespace

// ── CDC Record Creation Throughput ────────────────────────────────────

TEST(BenchmarkCDC, RecordCreationThroughput) {
    constexpr size_t kIters = 10000;
    constexpr size_t kWarmup = 500;

    uuid_u cluster = generate_uuid();
    uuid_u table = generate_uuid();
    auto shards = make_shard_ids(8);

    // Warmup
    for (size_t i = 0; i < kWarmup; ++i) {
        make_cdc_batch(cluster, table, shards, 0, 1);
    }

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        auto batch = make_cdc_batch(cluster, table, shards, 0, 100);
        auto end = std::chrono::high_resolution_clock::now();
        double us = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0;
        durations.push_back(us);
        EXPECT_EQ(100u, batch.size());
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("CDC: record creation (100 recs/batch)");

    // Records/second at mean: (100 / mean_us) * 1e6
    double recs_per_sec = (100.0 / stats.mean_us) * 1e6;
    std::cout << "  throughput=" << std::fixed << std::setprecision(0)
              << recs_per_sec << " recs/sec\n";

    EXPECT_GT(recs_per_sec, 0);
}

// ── CDC Event ID Deduplication Throughput ─────────────────────────────

TEST(BenchmarkCDC, EventIdDedupThroughput) {
    constexpr size_t kIters = 5000;
    constexpr size_t kWarmup = 200;

    uuid_u cluster = generate_uuid();
    uuid_u table = generate_uuid();
    auto shards = make_shard_ids(16);
    auto batch = make_cdc_batch(cluster, table, shards, 1, 500);

    // Warmup
    for (size_t i = 0; i < kWarmup; ++i) {
        std::set<ql::change_event_id_t, ql::change_event_id_compare_by_lsn_t> seen;
        for (const auto &r : batch) seen.insert(r.event_id);
    }

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        std::set<ql::change_event_id_t, ql::change_event_id_compare_by_lsn_t> seen;
        for (const auto &r : batch) seen.insert(r.event_id);
        auto end = std::chrono::high_resolution_clock::now();
        double us = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0;
        durations.push_back(us);
        EXPECT_EQ(500u, seen.size());
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("CDC: event_id dedup (500 unique IDs)");

    double ids_per_sec = (500.0 / stats.mean_us) * 1e6;
    std::cout << "  throughput=" << std::fixed << std::setprecision(0)
              << ids_per_sec << " ids/sec\n";
}

// ── CDC Change Record Roundtrip (Simulated Delivery) ──────────────────

TEST(BenchmarkCDC, ChangeRecordRoundtrip) {
    // Simulates the cost of serializing and comparing change records
    // that would flow through a changefeed delivery path.
    constexpr size_t kIters = 5000;
    constexpr size_t kBatchSize = 200;

    uuid_u cluster = generate_uuid();
    uuid_u table = generate_uuid();
    auto shards = make_shard_ids(32);

    // Warmup
    {
        auto b = make_cdc_batch(cluster, table, shards, 0, kBatchSize);
        volatile size_t sink = 0;
        for (const auto &r : b) {
            ql::change_record_t copy = r;
            if (copy.event_id < copy.event_id) ++sink;
        }
        (void)sink;
    }

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        auto batch = make_cdc_batch(cluster, table, shards, i * kBatchSize,
                                    kBatchSize);
        volatile size_t count = 0;

        auto start = std::chrono::high_resolution_clock::now();
        for (const auto &r : batch) {
            ql::change_record_t copy = r;
            if (copy.event_id < copy.event_id) ++count;  // never true
            (void)count;
        }
        auto end = std::chrono::high_resolution_clock::now();

        double us = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0;
        durations.push_back(us);
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("CDC: change-record roundtrip (200 recs)");

    double recs_per_sec = (static_cast<double>(kBatchSize) / stats.mean_us) * 1e6;
    std::cout << "  throughput=" << std::fixed << std::setprecision(0)
              << recs_per_sec << " recs/sec\n";
}

// ── CDC LSN Progression Throughput ────────────────────────────────────

TEST(BenchmarkCDC, LSNProgressionThroughput) {
    // Benchmarks LSN monotonicity check — critical for changefeed ordering.
    constexpr size_t kIters = 20000;
    constexpr size_t kWarmup = 500;

    std::vector<ql::log_sequence_number_t> lsns;
    lsns.reserve(1000);
    for (uint64_t i = 0; i < 1000; ++i) lsns.push_back({i});

    // Warmup
    for (size_t i = 0; i < kWarmup; ++i) {
        volatile bool ok = true;
        for (size_t j = 1; j < lsns.size(); ++j) {
            if (lsns[j] < lsns[j - 1]) ok = false;
        }
        (void)ok;
    }

    std::vector<double> durations;
    durations.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        volatile bool ok = true;
        for (size_t j = 1; j < lsns.size(); ++j) {
            if (lsns[j] < lsns[j - 1]) ok = false;
        }
        (void)ok;
        auto end = std::chrono::high_resolution_clock::now();
        double us = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0;
        durations.push_back(us);
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("CDC: LSN progression check (1000 LSNs)");
}

}  // namespace unittest
