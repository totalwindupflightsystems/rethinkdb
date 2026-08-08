// Copyright 2026 RethinkDB, all rights reserved.
//
// PERF-BENCH: Time-series storage benchmarks (spec §8.4).
//
// Measures:
//   - Write throughput: the chunked append-optimized write path
//     (time_series_ops_t::route_insert) vs a regular B-tree insert
//     (rdb_set) on a real on-disk store (TPTEST harness)
//   - Catalog append (select_chunk) vs sorted std::vector insert
//     (in-memory)
//   - between() read pruning: chunk-index overlap scan vs a full-index
//     walk on a 10K-chunk synthetic catalog, plus a store-level pruned
//     traversal (1 chunk tree) vs full traversal (all chunk trees)
//   - Retention overhead: expired_chunks() scan on a 10K-chunk catalog
//   - Downsample bucket-key generation throughput
//
// Uses std::chrono for timing via GTest harness — no Google Benchmark dep
// (same pattern as benchmark_cdc.cc / benchmark_vector.cc). These are
// informational benchmarks: assertions check correctness (row counts,
// candidate sets) or loose ratio bounds (pruned strictly faster than
// full), never host-dependent wall-clock thresholds.
//
// Note on the write comparison: per-op transaction overhead is identical
// on both sides (one SOFT-durability write txn per row); the chunked path
// additionally pays the catalog load/save and chunk routing. The spec's
// 2-5x claim (§10.1) is about I/O locality, not per-op CPU, so the
// numbers below are the honest per-row CPU cost of each path.

#include "unittest/gtest.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "arch/io/disk.hpp"
#include "btree/depth_first_traversal.hpp"
#include "btree/keys.hpp"
#include "btree/operations.hpp"
#include "btree/time_chunk.hpp"
#include "btree/time_series_config.hpp"
#include "btree/time_series_ops.hpp"
#include "buffer_cache/alt.hpp"
#include "buffer_cache/cache_balancer.hpp"
#include "concurrency/cond_var.hpp"
#include "containers/name_string.hpp"
#include "rdb_protocol/btree.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/pseudo_time.hpp"
#include "serializer/log/log_serializer.hpp"
#include "unittest/unittest_utils.hpp"
#include "utils.hpp"

namespace unittest {

namespace {

// ── Benchmark helpers (same pattern as benchmark_cdc.cc) ───────────────

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

// ── Store fixture (same pattern as time_series_test.cc) ────────────────

void with_ts_store(
        const std::function<void(cache_conn_t *, btree_slice_t *)> &body) {
    temp_file_t temp_file;
    io_backender_t io_backender(file_direct_io_mode_t::buffered_desired);
    filepath_file_opener_t file_opener(temp_file.name(), &io_backender);
    log_serializer_t::create(
        &file_opener, log_serializer_t::static_config_t());
    log_serializer_t serializer(
        log_serializer_t::dynamic_config_t(),
        &file_opener, &get_global_perfmon_collection());
    dummy_cache_balancer_t balancer(GIGABYTE);
    cache_t cache(&serializer, &balancer, &get_global_perfmon_collection());
    cache_conn_t cache_conn(&cache);

    {
        txn_t txn(&cache_conn, write_durability_t::HARD, 1);
        {
            buf_lock_t sb_lock(&txn, SUPERBLOCK_ID, alt_create_t::create);
            real_superblock_t superblock(std::move(sb_lock));
            btree_slice_t::init_real_superblock(
                &superblock, std::vector<char>(), binary_blob_t());
        }
        txn.commit();
    }

    btree_slice_t slice(&cache, &get_global_perfmon_collection(),
                        "ts_bench", index_type_t::PRIMARY);
    body(&cache_conn, &slice);
}

ql::datum_t ts_doc(const char *id, double epoch_seconds) {
    ql::datum_object_builder_t doc;
    doc.overwrite("id", ql::datum_t(datum_string_t(id)));
    doc.overwrite("ts", ql::pseudo::make_time(epoch_seconds, "+00:00"));
    return std::move(doc).to_datum();
}

/* One routed (chunked) insert inside its own write txn — the production
 * per-row write-path shape used by the store write benchmark. */
point_write_result_t routed_insert(
        cache_conn_t *cache_conn,
        btree_slice_t *slice,
        const ql::time_series_config_t &config,
        time_series_catalog_t *catalog,
        uint64_t chunk_target_rows,
        const store_key_t &key,
        const ql::datum_t &doc) {
    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_t> superblock;
    get_btree_superblock_and_txn_for_writing(
        cache_conn, nullptr, write_access_t::write, 4,
        write_durability_t::SOFT, &superblock, &txn);
    rdb_live_deletion_context_t deletion_context;
    point_write_response_t pw_res;
    rdb_modification_info_t mod_info;
    try {
        time_series_ops_t::route_insert(
            slice, superblock.get(), config, catalog, chunk_target_rows,
            key, doc, true /* overwrite */, repli_timestamp_t::distant_past,
            &deletion_context, &pw_res, &mod_info, nullptr);
    } catch (...) {
        superblock.reset();
        txn->commit();
        throw;
    }
    time_series_ops_t::save_catalog(superblock.get(), *catalog);
    superblock.reset();
    txn->commit();
    return pw_res.result;
}

/* One plain B-tree insert inside its own write txn (control path). */
void plain_insert(cache_conn_t *cache_conn, btree_slice_t *slice,
                  const store_key_t &key, const ql::datum_t &doc) {
    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_t> superblock;
    get_btree_superblock_and_txn_for_writing(
        cache_conn, nullptr, write_access_t::write, 4,
        write_durability_t::SOFT, &superblock, &txn);
    rdb_live_deletion_context_t deletion_context;
    point_write_response_t pw_res;
    rdb_modification_info_t mod_info;
    rdb_set(key, doc, true /* overwrite */, slice,
            repli_timestamp_t::distant_past, superblock.get(),
            &deletion_context, &pw_res, &mod_info, nullptr);
    superblock.reset();
    txn->commit();
}

/* Counts rows in one chunk tree (same pattern as time_series_test.cc). */
class counting_visitor_t : public depth_first_traversal_callback_t {
public:
    continue_bool_t handle_pair(
            UNUSED scoped_key_value_t &&keyvalue,
            UNUSED signal_t *interruptor) override {
        ++count;
        return continue_bool_t::CONTINUE;
    }
    size_t count = 0;
};

size_t count_tree_rows(cache_conn_t *cache_conn, block_id_t root) {
    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_t> superblock;
    get_btree_superblock_and_txn_for_reading(
        cache_conn, CACHE_SNAPSHOTTED_NO, &superblock, &txn);
    time_chunk_superblock_t read_sb(
        superblock.get(), root, superblock->get_stat_block_id());
    counting_visitor_t visitor;
    cond_t interruptor;
    btree_depth_first_traversal(
        &read_sb, key_range_t::universe(), &visitor,
        access_t::read, direction_t::FORWARD,
        release_superblock_t::KEEP, &interruptor);
    return visitor.count;
}

ql::time_series_config_t bench_config(uint64_t chunk_interval_seconds) {
    ql::time_series_config_t cfg;
    cfg.enabled = true;
    cfg.time_field = name_string_t::guarantee_valid("ts");
    cfg.chunk_interval_seconds = chunk_interval_seconds;
    cfg.retention_seconds = 0;
    return cfg;
}

}  // namespace

// ── In-memory: catalog append vs sorted-vector insert ──────────────────

TEST(BenchmarkTimeSeries, CatalogAppendVsSortedVectorInsert) {
    constexpr size_t kIters = 10000;
    constexpr size_t kWarmup = 200;

    /* A huge chunk interval: every in-order timestamp EXTENDS the newest
     * chunk (the pure append path, spec §4.1 step 2) instead of sealing a
     * new chunk — the honest O(1) append comparison. */
    ql::time_series_config_t config = bench_config(3600);
    const uint64_t base_us = 1000000000ULL;  // a fixed epoch

    /* Warmup */
    {
        ql::time_chunk_index_t idx;
        std::vector<uint64_t> sorted;
        sorted.reserve(kWarmup);
        for (size_t i = 0; i < kWarmup; ++i) {
            time_series_ops_t::select_chunk(
                &idx, config, std::numeric_limits<uint64_t>::max(),
                base_us + i * 1000000ULL);
            auto pos = std::lower_bound(sorted.begin(), sorted.end(),
                                        base_us + i * 1000000ULL);
            sorted.insert(pos, base_us + i * 1000000ULL);
        }
    }

    std::vector<double> chunked_us;
    std::vector<double> vector_us;
    chunked_us.reserve(kIters);
    vector_us.reserve(kIters);

    for (size_t i = 0; i < kIters; ++i) {
        const uint64_t ts_us = base_us + i * 1000000ULL;

        auto c_start = std::chrono::high_resolution_clock::now();
        ql::time_chunk_index_t idx;
        for (size_t j = 0; j < 100; ++j) {
            time_series_ops_t::select_chunk(
                &idx, config, std::numeric_limits<uint64_t>::max(),
                ts_us + j * 1000000ULL);
        }
        auto c_end = std::chrono::high_resolution_clock::now();
        EXPECT_EQ(1u, idx.chunks.size());  // all rows extended chunk 0

        auto v_start = std::chrono::high_resolution_clock::now();
        std::vector<uint64_t> sorted;
        sorted.reserve(100);
        for (size_t j = 0; j < 100; ++j) {
            auto pos = std::lower_bound(sorted.begin(), sorted.end(),
                                        ts_us + j * 1000000ULL);
            sorted.insert(pos, ts_us + j * 1000000ULL);
        }
        auto v_end = std::chrono::high_resolution_clock::now();

        chunked_us.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                c_end - c_start).count()) / 1000.0);
        vector_us.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                v_end - v_start).count()) / 1000.0);
    }

    auto c_stats = compute_stats(chunked_us, kIters);
    auto v_stats = compute_stats(vector_us, kIters);
    c_stats.print("TS: catalog append (100 extend ops)");
    v_stats.print("TS: sorted-vector insert (100 ops)");

    std::cout << "  append ops/sec=" << std::fixed << std::setprecision(0)
              << (100.0 / c_stats.mean_us) * 1e6 << "\n"
              << "  vector insert ops/sec="
              << (100.0 / v_stats.mean_us) * 1e6 << "\n";

    /* Loose sanity bound (task §8.4): the append path is O(1) per row and
     * must beat O(n) memmove insertion at 100 rows. */
    EXPECT_LE(c_stats.mean_us, v_stats.mean_us);
}

// ── In-memory: chunk-index pruning vs full-index walk ──────────────────

TEST(BenchmarkTimeSeries, ChunkIndexPruningVsFullScan) {
    constexpr size_t kChunks = 10000;
    constexpr size_t kQueries = 200;

    /* 10K hourly chunks tiling [0, 10000h). */
    ql::time_chunk_index_t idx;
    idx.chunks.reserve(kChunks);
    for (size_t i = 0; i < kChunks; ++i) {
        idx.chunks.emplace_back(
            i * 3600000000ULL, (i + 1) * 3600000000ULL, 1000, 77);
    }
    /* The catalog's total_rows sums the row counts (sanity). */
    EXPECT_EQ(kChunks * 1000u, time_series_ops_t::total_rows(idx));

    /* Warmup */
    for (size_t i = 0; i < 20; ++i) {
        idx.overlapping_chunks(i * 3600000000ULL,
                               (i + 1) * 3600000000ULL);
    }

    std::vector<double> pruned_us;
    std::vector<double> full_us;
    pruned_us.reserve(kQueries);
    full_us.reserve(kQueries);

    size_t pruned_total = 0;
    for (size_t q = 0; q < kQueries; ++q) {
        const uint64_t start_us = q * 10 * 3600000000ULL;
        const uint64_t end_us = start_us + 3600000000ULL;

        auto p_start = std::chrono::high_resolution_clock::now();
        std::vector<size_t> overlap = idx.overlapping_chunks(start_us, end_us);
        auto p_end = std::chrono::high_resolution_clock::now();
        pruned_total += overlap.size();

        auto f_start = std::chrono::high_resolution_clock::now();
        uint64_t full_rows = 0;
        for (const ql::time_chunk_bounds_t &c : idx.chunks) {
            full_rows += c.row_count;
        }
        auto f_end = std::chrono::high_resolution_clock::now();
        EXPECT_EQ(kChunks * 1000u, full_rows);

        pruned_us.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                p_end - p_start).count()) / 1000.0);
        full_us.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                f_end - f_start).count()) / 1000.0);
    }

    auto p_stats = compute_stats(pruned_us, kQueries);
    auto f_stats = compute_stats(full_us, kQueries);
    p_stats.print("TS: pruned overlap scan (1h window on 10K chunks)");
    f_stats.print("TS: full-index summary walk (10K chunks)");

    std::cout << "  pruned candidates/query=" << pruned_total / kQueries
              << "  full chunks/query=" << kChunks << "\n";

    /* The pruning signal: a 1h window selects 1 of 10K chunks (10,000x
     * fewer tree roots to traverse — spec §10.2). The overlap scan itself
     * is O(n) in the chunk count (linear scan, no early exit), so assert
     * on the candidate-set size, not the scan wall time. */
    EXPECT_EQ(1u, pruned_total / kQueries);
}

// ── In-memory: retention scan on a large catalog ───────────────────────

TEST(BenchmarkTimeSeries, RetentionExpiredChunksScan) {
    constexpr size_t kChunks = 10000;
    constexpr uint64_t kNowUs = 10000 * 3600000000ULL;
    constexpr uint64_t kRetentionSeconds = 7200;  // 2h

    /* 10K hourly chunks tiling [0, 10000h). With now=10000h and a 2h
     * retention the cutoff is 9998h: chunks 0..9996 (max_time < cutoff)
     * are expired, chunk 9997 (max_time == cutoff, exactly-at-TTL) is
     * kept, chunks 9998..9999 are live. */
    constexpr size_t kExpired = 9997;
    ql::time_chunk_index_t idx;
    idx.chunks.reserve(kChunks);
    for (size_t i = 0; i < kChunks; ++i) {
        idx.chunks.emplace_back(
            i * 3600000000ULL, (i + 1) * 3600000000ULL, 1000, 77);
    }

    /* Warmup */
    for (int i = 0; i < 5; ++i) {
        time_series_ops_t::expired_chunks(idx, kNowUs, kRetentionSeconds);
    }

    constexpr size_t kIters = 100;
    std::vector<double> durations;
    durations.reserve(kIters);
    size_t expired_count = 0;
    size_t last_expired = 0;
    for (size_t i = 0; i < kIters; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        std::vector<size_t> expired =
            time_series_ops_t::expired_chunks(idx, kNowUs, kRetentionSeconds);
        auto end = std::chrono::high_resolution_clock::now();
        expired_count = expired.size();
        last_expired = expired.empty() ? 0 : expired.back();
        durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);
    }
    EXPECT_EQ(kExpired, expired_count);
    /* Prefix semantics: the expired chunks are exactly indices 0..9996
     * (the exactly-at-TTL chunk is kept). */
    EXPECT_EQ(kExpired - 1, last_expired);

    auto stats = compute_stats(durations, kIters);
    stats.print("TS: expired_chunks scan (10K chunks, 9997 expired)");
    std::cout << "  scans/sec=" << std::fixed << std::setprecision(0)
              << (1.0 / stats.mean_us) * 1e6 << "\n";

    /* A retention window covering the whole catalog (cutoff clamped to 0)
     * expires nothing. */
    EXPECT_TRUE(time_series_ops_t::expired_chunks(
        idx, kNowUs, 36000000ULL /* 10000h == now */).empty());
}

// ── In-memory: downsample bucket-key generation ────────────────────────

TEST(BenchmarkTimeSeries, DownsampleBucketKeyGeneration) {
    constexpr size_t kIters = 20000;
    constexpr size_t kWarmup = 500;
    const uint64_t base_us = 1750000000000000ULL;

    for (size_t i = 0; i < kWarmup; ++i) {
        time_series_ops_t::downsample_bucket_key(base_us + i * 2000000ULL);
    }

    std::vector<double> durations;
    durations.reserve(kIters);
    store_key_t last;
    for (size_t i = 0; i < kIters; ++i) {
        const uint64_t bucket_us = base_us + i * 2000000ULL;
        auto start = std::chrono::high_resolution_clock::now();
        store_key_t key =
            time_series_ops_t::downsample_bucket_key(bucket_us);
        auto end = std::chrono::high_resolution_clock::now();
        durations.push_back(static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - start).count()) / 1000.0);
        /* 20-digit zero-padded keys stay lexicographically ordered. */
        EXPECT_EQ(20u, key.size());
        if (i > 0) {
            EXPECT_TRUE(last < key);
        }
        last = key;
    }

    auto stats = compute_stats(durations, kIters);
    stats.print("TS: downsample bucket-key generation");
    std::cout << "  keys/sec=" << std::fixed << std::setprecision(0)
              << (1.0 / stats.mean_us) * 1e6 << "\n";
}

// ── Store-level: chunked write path vs plain B-tree insert ─────────────

TPTEST(BenchmarkTimeSeries, StoreWriteThroughputChunkedVsPlain) {
    constexpr size_t kRows = 300;

    with_ts_store([&](cache_conn_t *cache_conn, btree_slice_t *slice) {
        ql::time_series_config_t config = bench_config(3600);
        time_series_catalog_t catalog;
        catalog.config = config;

        /* Chunked append path: in-order timestamps 1s apart, a huge
         * interval (3600s) and seal threshold → every row extends chunk 0
         * (the append-optimized hot path). */
        auto c_start = std::chrono::high_resolution_clock::now();
        for (size_t i = 0; i < kRows; ++i) {
            point_write_result_t res = routed_insert(
                cache_conn, slice, config, &catalog,
                std::numeric_limits<uint64_t>::max(),
                store_key_t(strprintf("c%04zu", i).c_str()),
                ts_doc(strprintf("c%04zu", i).c_str(), 1000.0 + i));
            EXPECT_EQ(point_write_result_t::STORED, res);
        }
        auto c_end = std::chrono::high_resolution_clock::now();
        EXPECT_EQ(kRows, time_series_ops_t::total_rows(catalog.chunk_index));
        EXPECT_EQ(1u, catalog.chunk_index.chunks.size());

        /* Plain B-tree insert: same doc shape, same per-op txn, no
         * catalog, no chunk routing. */
        auto p_start = std::chrono::high_resolution_clock::now();
        for (size_t i = 0; i < kRows; ++i) {
            plain_insert(cache_conn, slice,
                         store_key_t(strprintf("p%04zu", i).c_str()),
                         ts_doc(strprintf("p%04zu", i).c_str(), 1000.0 + i));
        }
        auto p_end = std::chrono::high_resolution_clock::now();

        double c_us = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                c_end - c_start).count());
        double p_us = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                p_end - p_start).count());

        std::cout << "BENCH: TS store write throughput ("
                  << kRows << " rows, per-op SOFT txn)\n"
                  << "  chunked path: " << std::fixed << std::setprecision(1)
                  << c_us / kRows << " us/op  "
                  << (kRows / c_us) * 1e6 << " ops/sec\n"
                  << "  plain path:  " << p_us / kRows << " us/op  "
                  << (kRows / p_us) * 1e6 << " ops/sec\n";
    });
}

// ── Store-level: pruned chunk traversal vs full traversal ──────────────

TPTEST(BenchmarkTimeSeries, StoreReadPruningVsFullScan) {
    constexpr size_t kChunks = 200;

    with_ts_store([&](cache_conn_t *cache_conn, btree_slice_t *slice) {
        /* chunk_interval=2s with rows 1s apart → 2 rows per chunk. */
        ql::time_series_config_t config = bench_config(2);
        time_series_catalog_t catalog;
        catalog.config = config;
        for (size_t i = 0; i < kChunks * 2; ++i) {
            routed_insert(
                cache_conn, slice, config, &catalog,
                std::numeric_limits<uint64_t>::max(),
                store_key_t(strprintf("s%04zu", i).c_str()),
                ts_doc(strprintf("s%04zu", i).c_str(), 1000.0 + i));
        }
        ASSERT_EQ(kChunks, catalog.chunk_index.chunks.size());

        /* Pruned: traverse only the chunk overlapping a 2s window. */
        const uint64_t start_us = 1000000000ULL + 40 * 1000000ULL;
        std::vector<size_t> overlap = catalog.chunk_index.overlapping_chunks(
            start_us, start_us + 2000000ULL);
        ASSERT_EQ(1u, overlap.size());
        block_id_t pruned_root =
            catalog.chunk_index.chunks[overlap[0]].root_block;

        auto pr_start = std::chrono::high_resolution_clock::now();
        size_t pruned_rows = count_tree_rows(cache_conn, pruned_root);
        auto pr_end = std::chrono::high_resolution_clock::now();

        auto full_start = std::chrono::high_resolution_clock::now();
        size_t full_rows = 0;
        for (const ql::time_chunk_bounds_t &c : catalog.chunk_index.chunks) {
            full_rows += count_tree_rows(cache_conn, c.root_block);
        }
        auto full_end = std::chrono::high_resolution_clock::now();

        EXPECT_EQ(2u, pruned_rows);
        EXPECT_EQ(kChunks * 2u, full_rows);

        double pr_us = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                pr_end - pr_start).count());
        double full_us = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                full_end - full_start).count());

        std::cout << "BENCH: TS store read traversal ("
                  << kChunks << " chunks x 2 rows)\n"
                  << "  pruned (1 chunk tree): " << std::fixed
                  << std::setprecision(1) << pr_us << " us for "
                  << pruned_rows << " rows\n"
                  << "  full (" << kChunks
                  << " chunk trees):  " << full_us << " us for "
                  << full_rows << " rows\n";

        /* Loose sanity bound: the pruned read touches 1/200th of the
         * chunk trees and must be strictly faster end to end. */
        EXPECT_LT(pr_us, full_us);
    });
}

}  // namespace unittest
