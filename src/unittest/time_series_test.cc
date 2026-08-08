// Copyright 2026 RethinkDB, all rights reserved.
/* PHASE3-TS-1 / spec §8.1:
 *   - chunk index: insert, overlap query, boundary conditions
 *   - serialization: round-trip time_series_config_t, time_chunk_index_t
 *   - downsample selection: correct resolution for range sizes
 *   - retention validation: exactly-at-TTL, just-before, just-after
 * Plus table_config_t integration (field serialization + equality). */
#include "btree/time_series_config.hpp"
#include "btree/time_chunk.hpp"
#include "btree/time_series_ops.hpp"
#include "rdb_protocol/datum_stream/readgens.hpp"
#include "rdb_protocol/protocol.hpp"
#include "rdb_protocol/pseudo_time.hpp"
#include "rdb_protocol/terms/time_series.hpp"

#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "arch/io/disk.hpp"
#include "btree/depth_first_traversal.hpp"
#include "btree/operations.hpp"
#include "buffer_cache/alt.hpp"
#include "buffer_cache/cache_balancer.hpp"
#include "clustering/administration/tables/table_metadata.hpp"
#include "concurrency/cond_var.hpp"
#include "containers/archive/vector_stream.hpp"
#include "containers/archive/versioned.hpp"
#include "containers/binary_blob.hpp"
#include "containers/name_string.hpp"
#include "containers/uuid.hpp"
#include "rdb_protocol/btree.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/error.hpp"
#include "rdb_protocol/lazy_btree_val.hpp"
#include "rdb_protocol/minidriver.hpp"
#include "rdb_protocol/protocol.hpp"
#include "rdb_protocol/pseudo_time.hpp"
#include "rdb_protocol/time_series_errors.hpp"
#include "serializer/log/log_serializer.hpp"
#include "unittest/gtest.hpp"
#include "unittest/unittest_utils.hpp"
#include "utils.hpp"

namespace unittest {

namespace {

template <class T>
T round_trip(const T &in) {
    write_message_t wm;
    serialize<cluster_version_t::LATEST_DISK>(&wm, in);
    vector_stream_t stream;
    EXPECT_EQ(0, send_write_message(&stream, &wm));
    std::vector<char> data = stream.vector();
    vector_read_stream_t rs(std::move(data));
    T out;
    EXPECT_EQ(archive_result_t::SUCCESS,
              deserialize<cluster_version_t::LATEST_DISK>(&rs, &out));
    return out;
}

/* Assert that `fn` throws a ql exception whose message contains `fragment`. */
template <class F>
void expect_reql_error(const char *fragment, F &&fn) {
    try {
        fn();
        ADD_FAILURE() << "expected an exception containing: " << fragment;
    } catch (const ql::base_exc_t &e) {
        std::string msg = e.what();
        EXPECT_NE(std::string::npos, msg.find(fragment))
            << "actual message: " << msg;
    }
}

ql::downsample_step_t make_step(uint64_t age, uint64_t to) {
    ql::downsample_step_t step;
    step.age_seconds = age;
    step.target_interval_seconds = to;
    /* Aggregates stay empty in unit tests (wire_func_t requires compiled
     * ReQL terms; that path is covered by the live E2E). */
    return step;
}

ql::time_series_config_t make_ts_config() {
    ql::time_series_config_t cfg;
    cfg.enabled = true;
    cfg.time_field = name_string_t::guarantee_valid("ts");
    cfg.chunk_interval_seconds = 3600;
    cfg.retention_seconds = 7776000;  // 90d
    cfg.downsample_steps.push_back(make_step(86400, 60));      // 24h -> 1m
    cfg.downsample_steps.push_back(make_step(604800, 3600));   // 7d -> 1h
    cfg.downsample_steps.push_back(make_step(2592000, 86400)); // 30d -> 1d
    return cfg;
}

}  // namespace

TEST(TimeSeries, ConfigRoundTrip) {
    ql::time_series_config_t cfg = make_ts_config();
    ql::time_series_config_t out = round_trip(cfg);
    EXPECT_TRUE(cfg == out);

    /* Default-constructed (disabled) config must also round-trip. */
    ql::time_series_config_t empty;
    EXPECT_TRUE(empty == round_trip(empty));
}

TEST(TimeSeries, TableConfigRoundTrip) {
    /* The config rides inside table_config_t (Raft metadata); prove the
    optional field serializes and round-trips through the whole struct. */
    table_config_t tc;
    tc.basic.name = name_string_t::guarantee_valid("ts");
    tc.basic.database = generate_uuid();
    tc.basic.primary_key = "id";
    tc.write_ack_config = write_ack_config_t::MAJORITY;
    tc.durability = write_durability_t::HARD;
    tc.time_series_config = make_optional(make_ts_config());

    table_config_t out = round_trip(tc);
    EXPECT_TRUE(tc == out);
    EXPECT_TRUE(out.time_series_config.has_value());
    EXPECT_TRUE(out.time_series_config->enabled);
    EXPECT_EQ(std::string("ts"),
              out.time_series_config->time_field.str());

    /* A plain table (no time-series) round-trips with nullopt preserved.
    (write_ack_config/durability are always set by the server before
    serialization; set them here so the enum bytes are initialized.) */
    table_config_t plain;
    plain.basic.name = name_string_t::guarantee_valid("plain");
    plain.basic.database = generate_uuid();
    plain.basic.primary_key = "id";
    plain.write_ack_config = write_ack_config_t::MAJORITY;
    plain.durability = write_durability_t::HARD;
    table_config_t plain_out = round_trip(plain);
    EXPECT_TRUE(plain == plain_out);
    EXPECT_FALSE(plain_out.time_series_config.has_value());

    /* Equality distinguishes engaged vs not. */
    EXPECT_FALSE(plain == tc);
}

TEST(TimeSeries, ChunkIndexRoundTrip) {
    ql::time_chunk_index_t idx;
    idx.chunks.push_back(ql::time_chunk_bounds_t{0, 3600000000ULL, 1200});
    idx.chunks.push_back(ql::time_chunk_bounds_t{3600000000ULL,
                                                 7200000000ULL, 900});
    ql::time_chunk_index_t out = round_trip(idx);
    EXPECT_TRUE(idx == out);
    EXPECT_EQ(2u, out.chunks.size());
    EXPECT_EQ(900u, out.chunks[1].row_count);
}

TEST(TimeSeries, ChunkOverlappingChunks) {
    ql::time_chunk_index_t idx;
    /* Three adjacent half-open chunks: [0,100), [100,200), [200,300). */
    idx.chunks.push_back(ql::time_chunk_bounds_t{0, 100, 10});
    idx.chunks.push_back(ql::time_chunk_bounds_t{100, 200, 20});
    idx.chunks.push_back(ql::time_chunk_bounds_t{200, 300, 30});

    /* Empty / inverted ranges return nothing. */
    EXPECT_EQ(std::vector<size_t>({}), idx.overlapping_chunks(50, 50));
    EXPECT_EQ(std::vector<size_t>({}), idx.overlapping_chunks(60, 50));

    /* Exact boundary: [100,200) touches chunk 1 only (max is exclusive). */
    EXPECT_EQ(std::vector<size_t>({1}), idx.overlapping_chunks(100, 200));
    /* [0,100) touches chunk 0 only. */
    EXPECT_EQ(std::vector<size_t>({0}), idx.overlapping_chunks(0, 100));

    /* Spanning all chunks. */
    EXPECT_EQ(std::vector<size_t>({0, 1, 2}),
              idx.overlapping_chunks(0, 300));
    /* Spanning beyond both ends. */
    EXPECT_EQ(std::vector<size_t>({0, 1, 2}),
              idx.overlapping_chunks(0, 1000));

    /* Disjoint (after the last chunk, and before the first). */
    EXPECT_EQ(std::vector<size_t>({}), idx.overlapping_chunks(300, 400));
    EXPECT_EQ(std::vector<size_t>({}), idx.overlapping_chunks(400, 500));

    /* Partial overlap straddling two chunks. */
    EXPECT_EQ(std::vector<size_t>({0, 1}), idx.overlapping_chunks(50, 150));
    EXPECT_EQ(std::vector<size_t>({1, 2}), idx.overlapping_chunks(150, 250));

    /* Single-point range inside a chunk. */
    EXPECT_EQ(std::vector<size_t>({1}), idx.overlapping_chunks(100, 101));

    /* Empty index. */
    ql::time_chunk_index_t empty;
    EXPECT_EQ(std::vector<size_t>({}), empty.overlapping_chunks(0, 100));
}

/* PHASE3-TS-3: between-window normalization. The row filter and the chunk
 * pruning both consume the half-open [start_us, end_us) normalization. */
TEST(TimeSeries, TsBetweenRangeNormalize) {
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    uint64_t start = 0, end = 0;
    auto norm = [&](const ts_between_range_t &r) {
        normalize_ts_range(r, &start, &end);
    };

    /* Fully unbounded (minval..maxval): everything. */
    ts_between_range_t unbounded;
    norm(unbounded);
    EXPECT_EQ(0u, start);
    EXPECT_EQ(max, end);

    /* Default between [40, 80): left closed, right open. */
    ts_between_range_t half_open;
    half_open.has_left = true;
    half_open.start_us = 40;
    half_open.has_right = true;
    half_open.end_us = 80;
    half_open.right_open = true;
    norm(half_open);
    EXPECT_EQ(40u, start);
    EXPECT_EQ(80u, end);

    /* Left-open (40, 80): start shifts by one micro. */
    ts_between_range_t left_open = half_open;
    left_open.left_open = true;
    norm(left_open);
    EXPECT_EQ(41u, start);
    EXPECT_EQ(80u, end);

    /* Right-closed [40, 80]: end exclusive-incremented. */
    ts_between_range_t right_closed = half_open;
    right_closed.right_open = false;
    norm(right_closed);
    EXPECT_EQ(40u, start);
    EXPECT_EQ(81u, end);

    /* Closed-closed equal bounds [t, t]: single micro point. */
    ts_between_range_t point = half_open;
    point.end_us = 40;
    point.right_open = false;
    norm(point);
    EXPECT_EQ(40u, start);
    EXPECT_EQ(41u, end);

    /* Open-closed equal bounds (t, t]: empty. */
    ts_between_range_t empty_pt = point;
    empty_pt.left_open = true;
    norm(empty_pt);
    EXPECT_EQ(41u, start);
    EXPECT_EQ(41u, end);

    /* Inverted [80, 40): empty. */
    ts_between_range_t inverted = half_open;
    inverted.start_us = 80;
    inverted.end_us = 40;
    norm(inverted);
    EXPECT_EQ(80u, start);
    EXPECT_EQ(40u, end);

    /* Unbounded left (.., 70): start stays 0. */
    ts_between_range_t no_left = half_open;
    no_left.has_left = false;
    no_left.end_us = 70;
    norm(no_left);
    EXPECT_EQ(0u, start);
    EXPECT_EQ(70u, end);

    /* Unbounded right [130, ..): end stays UINT64_MAX. */
    ts_between_range_t no_right = half_open;
    no_right.has_right = false;
    no_right.start_us = 130;
    norm(no_right);
    EXPECT_EQ(130u, start);
    EXPECT_EQ(max, end);

    /* Overflow guards: no increment past UINT64_MAX. */
    ts_between_range_t sat = half_open;
    sat.start_us = max;
    sat.left_open = true;
    sat.end_us = max;
    sat.right_open = false;
    norm(sat);
    EXPECT_EQ(max, start);
    EXPECT_EQ(max, end);
}

/* PHASE3-TS-3: default-`between` dispatch predicate — bounds must be a
 * time-range over a time-series table (times and/or minval/maxval, at
 * least one actual time). */
TEST(TimeSeries, TsBoundsAreTimeLike) {
    ql::datum_t t1 = ql::pseudo::make_time(1000.0, "+00:00");
    ql::datum_t t2 = ql::pseudo::make_time(2000.0, "+00:00");

    /* Time/time — the plain between on a time-series table. */
    EXPECT_TRUE(ql::ts_bounds_are_time_like(ql::datumspec_t(
        ql::datum_range_t(t1, key_range_t::closed,
                          t2, key_range_t::open))));
    /* Time with an unbounded end. */
    EXPECT_TRUE(ql::ts_bounds_are_time_like(ql::datumspec_t(
        ql::datum_range_t(ql::datum_t::minval(), key_range_t::closed,
                          t2, key_range_t::open))));
    EXPECT_TRUE(ql::ts_bounds_are_time_like(ql::datumspec_t(
        ql::datum_range_t(t1, key_range_t::closed,
                          ql::datum_t::maxval(), key_range_t::open))));
    /* Non-time bounds are not time-like (pkey between keeps its semantics). */
    EXPECT_FALSE(ql::ts_bounds_are_time_like(ql::datumspec_t(
        ql::datum_range_t(ql::datum_t(5.0), key_range_t::closed,
                          ql::datum_t(10.0), key_range_t::open))));
    EXPECT_FALSE(ql::ts_bounds_are_time_like(ql::datumspec_t(
        ql::datum_range_t(t1, key_range_t::closed,
                          ql::datum_t(10.0), key_range_t::open))));
    EXPECT_FALSE(ql::ts_bounds_are_time_like(ql::datumspec_t(
        ql::datum_range_t(ql::datum_t(5.0), key_range_t::closed,
                          t2, key_range_t::open))));
    /* Both extrema but no time: plain full-scan between. */
    EXPECT_FALSE(ql::ts_bounds_are_time_like(ql::datumspec_t(
        ql::datum_range_t(ql::datum_t::minval(), key_range_t::closed,
                          ql::datum_t::maxval(), key_range_t::open))));
    /* Universe (open minval..maxval): full scan, not a time between. */
    EXPECT_FALSE(ql::ts_bounds_are_time_like(ql::datumspec_t(
        ql::datum_range_t::universe())));
    /* Empty range: not time-like. */
    EXPECT_FALSE(ql::ts_bounds_are_time_like(ql::datumspec_t(
        ql::datum_range_t(t2, key_range_t::closed,
                          t1, key_range_t::open))));
}

TEST(TimeSeries, DownsampleSelection) {
    ql::time_series_config_t cfg = make_ts_config();
    /* Steps (target_interval desc): 30d->1d (age 2592000), 7d->1h (age
    604800), 24h->1m (age 86400). */

    /* Ranges at or below the finest step's age select nothing. */
    EXPECT_EQ(nullptr, cfg.select_downsample(1));
    EXPECT_EQ(nullptr, cfg.select_downsample(86399));
    EXPECT_EQ(nullptr, cfg.select_downsample(86400));  // must EXCEED age

    /* Just past the finest step's age -> 1m resolution. */
    const ql::downsample_step_t *s = cfg.select_downsample(86401);
    ASSERT_TRUE(s != nullptr);
    EXPECT_EQ(60u, s->target_interval_seconds);
    EXPECT_EQ(&cfg.downsample_steps[0], s);

    /* Just past the 7d step's age -> 1h resolution. */
    s = cfg.select_downsample(604801);
    ASSERT_TRUE(s != nullptr);
    EXPECT_EQ(3600u, s->target_interval_seconds);
    EXPECT_EQ(&cfg.downsample_steps[1], s);

    /* Just past the 30d step's age -> 1d resolution. */
    s = cfg.select_downsample(2592001);
    ASSERT_TRUE(s != nullptr);
    EXPECT_EQ(86400u, s->target_interval_seconds);
    EXPECT_EQ(&cfg.downsample_steps[2], s);

    /* Huge range -> coarsest step. */
    s = cfg.select_downsample(31536000);
    ASSERT_TRUE(s != nullptr);
    EXPECT_EQ(86400u, s->target_interval_seconds);

    /* Selection is independent of insertion order. */
    ql::time_series_config_t scrambled;
    scrambled.enabled = true;
    scrambled.time_field = name_string_t::guarantee_valid("ts");
    scrambled.downsample_steps.push_back(make_step(2592000, 86400));
    scrambled.downsample_steps.push_back(make_step(86400, 60));
    scrambled.downsample_steps.push_back(make_step(604800, 3600));
    s = scrambled.select_downsample(604801);
    ASSERT_TRUE(s != nullptr);
    EXPECT_EQ(3600u, s->target_interval_seconds);

    /* No steps -> always nullptr. */
    ql::time_series_config_t bare;
    EXPECT_EQ(nullptr, bare.select_downsample(1ULL << 40));
}

/* ── PHASE3-TS-5: downsample storage (spec §5.3) ───────────────────────── */

TEST(TimeSeries, DownsampleRootsRoundTrip) {
    /* The catalog blob carries the per-step downsample roots alongside the
     * chunk index; both must round-trip together (they commit atomically —
     * catalog + data in one blob). */
    time_series_catalog_t catalog;
    catalog.config = make_ts_config();
    catalog.chunk_index.chunks.push_back(
        ql::time_chunk_bounds_t{0, 3600000000ULL, 100, 77});
    catalog.downsample_roots.push_back(
        downsample_root_t(60, 101));
    catalog.downsample_roots.push_back(
        downsample_root_t(3600, 202));
    catalog.downsample_roots.push_back(
        downsample_root_t(86400, 303));

    time_series_catalog_t out = round_trip(catalog);
    EXPECT_TRUE(catalog == out);
    ASSERT_EQ(3u, out.downsample_roots.size());
    EXPECT_EQ(3600u, out.downsample_roots[1].target_interval_seconds);
    EXPECT_EQ(202, out.downsample_roots[1].root_block);

    /* Default catalog (no downsample section) round-trips with an empty
     * vector. */
    time_series_catalog_t plain;
    EXPECT_TRUE(plain == round_trip(plain));
    EXPECT_TRUE(round_trip(plain).downsample_roots.empty());
}

TEST(TimeSeries, DownsampleBucketKeyOrdering) {
    /* Keys are 20-digit zero-padded decimal strings of the bucket start, so
     * lexicographic order == chronological order. */
    const store_key_t k0 =
        time_series_ops_t::downsample_bucket_key(0);
    const store_key_t k1 =
        time_series_ops_t::downsample_bucket_key(60000000ULL);
    const store_key_t k9 =
        time_series_ops_t::downsample_bucket_key(90000000ULL);
    const store_key_t k10 =
        time_series_ops_t::downsample_bucket_key(100000000ULL);
    const store_key_t kmax =
        time_series_ops_t::downsample_bucket_key(
            std::numeric_limits<uint64_t>::max());

    EXPECT_EQ(20u, k1.size());
    EXPECT_EQ(std::string("00000000000000000000"),
              std::string(k0.contents(), k0.contents() + k0.size()));
    EXPECT_EQ(std::string("00000000000060000000"),
              std::string(k1.contents(), k1.contents() + k1.size()));
    EXPECT_EQ(std::string("00000000000090000000"),
              std::string(k9.contents(), k9.contents() + k9.size()));
    EXPECT_EQ(std::string("00000000000100000000"),
              std::string(k10.contents(), k10.contents() + k10.size()));
    /* Numeric order must equal string order (the 20-digit width is what
     * guarantees this — "00000000000100000000" > "00000000000090000000"
     * lexicographically AND numerically). */
    EXPECT_TRUE(k0 < k1);
    EXPECT_TRUE(k9 < k10);
    EXPECT_TRUE(k10 < kmax);
}

TEST(TimeSeries, DownsampleKeyRangeExact) {
    /* The bucket-key range for a window covers exactly the buckets that
     * overlap it. Use a 60s interval (= 60M micros). */
    const uint64_t interval = 60;
    const uint64_t us = 1000000ULL;  // micros per second

    /* Window [0, 120s) covers buckets 0 (0-60s) and 60 (60-120s). */
    key_range_t r = time_series_ops_t::downsample_key_range(
        0, 120 * us, interval);
    EXPECT_TRUE(r.contains_key(
        time_series_ops_t::downsample_bucket_key(0)));
    EXPECT_TRUE(r.contains_key(
        time_series_ops_t::downsample_bucket_key(60 * us)));
    EXPECT_FALSE(r.contains_key(
        time_series_ops_t::downsample_bucket_key(120 * us)));

    /* Window [30s, 150s) straddles buckets 0, 60, 120. */
    r = time_series_ops_t::downsample_key_range(
        30 * us, 150 * us, interval);
    EXPECT_TRUE(r.contains_key(
        time_series_ops_t::downsample_bucket_key(0)));
    EXPECT_TRUE(r.contains_key(
        time_series_ops_t::downsample_bucket_key(60 * us)));
    EXPECT_TRUE(r.contains_key(
        time_series_ops_t::downsample_bucket_key(120 * us)));
    EXPECT_FALSE(r.contains_key(
        time_series_ops_t::downsample_bucket_key(180 * us)));

    /* Window exactly one bucket [60s, 120s) → only bucket 60. */
    r = time_series_ops_t::downsample_key_range(
        60 * us, 120 * us, interval);
    EXPECT_FALSE(r.contains_key(
        time_series_ops_t::downsample_bucket_key(0)));
    EXPECT_TRUE(r.contains_key(
        time_series_ops_t::downsample_bucket_key(60 * us)));
    EXPECT_FALSE(r.contains_key(
        time_series_ops_t::downsample_bucket_key(120 * us)));

    /* Window inside one bucket [61s, 119s) → bucket 60 only. */
    r = time_series_ops_t::downsample_key_range(
        61 * us, 119 * us, interval);
    EXPECT_TRUE(r.contains_key(
        time_series_ops_t::downsample_bucket_key(60 * us)));
    EXPECT_FALSE(r.contains_key(
        time_series_ops_t::downsample_bucket_key(0)));
    EXPECT_FALSE(r.contains_key(
        time_series_ops_t::downsample_bucket_key(120 * us)));

    /* Empty / inverted windows → empty range. */
    EXPECT_TRUE(time_series_ops_t::downsample_key_range(
        100 * us, 100 * us, interval).is_empty());
    EXPECT_TRUE(time_series_ops_t::downsample_key_range(
        200 * us, 100 * us, interval).is_empty());

    /* Unbounded right edge → every bucket at/after the first. */
    r = time_series_ops_t::downsample_key_range(
        90 * us, std::numeric_limits<uint64_t>::max(), interval);
    EXPECT_TRUE(r.contains_key(
        time_series_ops_t::downsample_bucket_key(60 * us)));
    EXPECT_TRUE(r.contains_key(
        time_series_ops_t::downsample_bucket_key(120 * us)));
    EXPECT_TRUE(r.contains_key(
        time_series_ops_t::downsample_bucket_key(1000000 * us)));
}

TEST(TimeSeries, DownsampleSelectionRoot) {
    /* §4.3 planner auto-selection: a window whose range exceeds a step's
     * age selects that step's tree; otherwise (or when the tree has no data
     * yet) the raw chunk-root path is kept. */
    time_series_catalog_t catalog;
    catalog.config = make_ts_config();  // ages 86400 / 604800 / 2592000
    /* Parallel roots: step 0 (1m) has data; step 1 (1h) has data; step 2
     * (1d) has a NULL root (step configured, nothing merged yet). */
    catalog.downsample_roots.push_back(downsample_root_t(60, 111));
    catalog.downsample_roots.push_back(downsample_root_t(3600, 222));
    catalog.downsample_roots.push_back(downsample_root_t(86400));

    /* Window shorter than the finest step's age → no downsample. */
    EXPECT_EQ(nullptr, time_series_ops_t::select_downsample_root(
        catalog, 0, 1000 * 1000000ULL));

    /* Just past the 24h age → the 1m step's tree. */
    const downsample_root_t *root =
        time_series_ops_t::select_downsample_root(
            catalog, 0, 86401 * 1000000ULL);
    ASSERT_TRUE(root != nullptr);
    EXPECT_EQ(60u, root->target_interval_seconds);
    EXPECT_EQ(111, root->root_block);

    /* Past the 7d age → the 1h step's tree. */
    root = time_series_ops_t::select_downsample_root(
        catalog, 0, 604801 * 1000000ULL);
    ASSERT_TRUE(root != nullptr);
    EXPECT_EQ(3600u, root->target_interval_seconds);

    /* Past the 30d age the selected step is step 2, whose tree has no rows
     * yet → fall back to raw (never return empty for existing raw data). */
    EXPECT_EQ(nullptr, time_series_ops_t::select_downsample_root(
        catalog, 0, 2592001 * 1000000ULL));

    /* A catalog whose roots vector is shorter than the step list (merge
     * never ran) → fall back to raw. */
    time_series_catalog_t no_roots;
    no_roots.config = make_ts_config();
    EXPECT_EQ(nullptr, time_series_ops_t::select_downsample_root(
        no_roots, 0, 86401 * 1000000ULL));

    /* Empty window → no selection. */
    EXPECT_EQ(nullptr, time_series_ops_t::select_downsample_root(
        catalog, 5000 * 1000000ULL, 5000 * 1000000ULL));
    EXPECT_EQ(nullptr, time_series_ops_t::select_downsample_root(
        catalog, 5000 * 1000000ULL, 1000 * 1000000ULL));

    /* Non-time-series config (disabled) → no selection. */
    time_series_catalog_t plain;
    EXPECT_EQ(nullptr, time_series_ops_t::select_downsample_root(
        plain, 0, 9999999 * 1000000ULL));
}

TEST(TimeSeries, DownsampleCandidates) {
    /* Sealed, non-empty, unmerged chunks are merge candidates; the active
     * newest chunk, merged chunks, and empty chunks are not. Chunk 0 (10
     * rows) and chunk 2 (30 rows) are both sealed and unmerged, so both are
     * candidates; chunk 1 is merged, chunk 3 is empty, chunk 4 is newest. */
    ql::time_chunk_index_t idx;
    idx.chunks.push_back(ql::time_chunk_bounds_t{0, 100, 10});
    idx.chunks.push_back(ql::time_chunk_bounds_t{100, 200, 20});
    idx.chunks.push_back(ql::time_chunk_bounds_t{200, 300, 30});
    idx.chunks[1].merged = true;                 // already folded
    idx.chunks.push_back(ql::time_chunk_bounds_t{300, 400, 0});  // empty
    idx.chunks.push_back(ql::time_chunk_bounds_t{400, 500, 40});  // newest

    EXPECT_EQ(std::vector<size_t>({0, 2}),
              time_series_ops_t::downsample_candidates(idx));

    /* A single chunk (no sealed chunks) → nothing. */
    ql::time_chunk_index_t single;
    single.chunks.push_back(ql::time_chunk_bounds_t{0, 100, 10});
    EXPECT_EQ(std::vector<size_t>({}),
              time_series_ops_t::downsample_candidates(single));

    /* Empty index → nothing. */
    EXPECT_EQ(std::vector<size_t>({}),
              time_series_ops_t::downsample_candidates(
                  ql::time_chunk_index_t()));
}

TEST(TimeSeries, RetentionBoundary) {
    /* Unit-test configs carry no wire funcs, so drop the steps: the
    retention checks are independent of the downsample pipeline. */
    /* Exactly at TTL is accepted (spec: *exceeds* 365d is the error). */
    ql::time_series_config_t at_ttl = make_ts_config();
    at_ttl.downsample_steps.clear();
    at_ttl.retention_seconds = 31536000;
    EXPECT_NO_THROW(at_ttl.validate_or_throw());

    /* Just before TTL is accepted. */
    ql::time_series_config_t before_ttl = make_ts_config();
    before_ttl.downsample_steps.clear();
    before_ttl.retention_seconds = 31535999;
    EXPECT_NO_THROW(before_ttl.validate_or_throw());

    /* Just after TTL is rejected with the spec's message. */
    ql::time_series_config_t after_ttl = make_ts_config();
    after_ttl.downsample_steps.clear();
    after_ttl.retention_seconds = 31536001;
    expect_reql_error("Retention period exceeds maximum allowed (365d).",
                      [&] { after_ttl.validate_or_throw(); });
}

TEST(TimeSeries, ValidateRejectsBadConfig) {
    /* Missing time field on an enabled config. */
    ql::time_series_config_t no_field;
    no_field.enabled = true;
    expect_reql_error("Time-series field must exist in every document.",
                      [&] { no_field.validate_or_throw(); });

    /* chunk_interval below 1. */
    ql::time_series_config_t bad_chunk = make_ts_config();
    bad_chunk.chunk_interval_seconds = 0;
    expect_reql_error("chunk_interval",
                      [&] { bad_chunk.validate_or_throw(); });

    /* Overlapping downsample ages. */
    ql::time_series_config_t overlap = make_ts_config();
    overlap.downsample_steps.push_back(make_step(86400, 300));
    expect_reql_error("Downsample age ranges must not overlap.",
                      [&] { overlap.validate_or_throw(); });

    /* Empty aggregate. */
    ql::time_series_config_t no_agg = make_ts_config();
    no_agg.downsample_steps.clear();
    no_agg.downsample_steps.push_back(make_step(86400, 60));
    expect_reql_error("Downsample `aggregate` must not be empty.",
                      [&] { no_agg.validate_or_throw(); });
}

/* ── PHASE3-TS-2: chunked storage + append-optimized write path ─────────── */

namespace {

/* Real on-disk harness (same pattern as partition_ops_test): a fresh
 * serializer + cache + primary superblock, then `body` runs with a write
 * connection and a btree slice for rdb_set. */
void with_ts_store(
        const std::function<void(cache_conn_t *, btree_slice_t *)> &body) {
    temp_file_t temp_file;
    io_backender_t io_backender(file_direct_io_mode_t::buffered_desired);
    filepath_file_opener_t file_opener(temp_file.name(), &io_backender);
    log_serializer_t::create(
        &file_opener,
        log_serializer_t::static_config_t());
    log_serializer_t serializer(
        log_serializer_t::dynamic_config_t(),
        &file_opener,
        &get_global_perfmon_collection());
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
                        "ts_test", index_type_t::PRIMARY);
    body(&cache_conn, &slice);
}

/* Build a document {id, ts} with `ts` a ReQL TIME pseudo-type. */
ql::datum_t ts_doc(const char *id, double epoch_seconds) {
    ql::datum_object_builder_t doc;
    doc.overwrite("id", ql::datum_t(datum_string_t(id)));
    doc.overwrite("ts", ql::pseudo::make_time(epoch_seconds, "+00:00"));
    return std::move(doc).to_datum();
}

/* Run a routed insert inside a write txn. Returns the point-write result. */
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
        /* Error paths (bad time field) reject before any chunk mutation;
        commit the empty txn to release the superblock cleanly. */
        superblock.reset();
        txn->commit();
        throw;
    }
    time_series_ops_t::save_catalog(superblock.get(), *catalog);
    /* The superblock holds a live page acq (sb_buf_); it must be released
     * before the txn commits (same pattern as btree_whole.cc run_txn_fn). */
    superblock.reset();
    txn->commit();
    return pw_res.result;
}

/* Load the catalog in a fresh read txn. */
time_series_catalog_t load_ts_catalog(cache_conn_t *cache_conn) {
    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_t> superblock;
    get_btree_superblock_and_txn_for_reading(
        cache_conn, CACHE_SNAPSHOTTED_NO, &superblock, &txn);
    return time_series_ops_t::load_catalog(superblock.get());
}

ql::time_series_config_t make_write_config() {
    ql::time_series_config_t cfg;
    cfg.enabled = true;
    cfg.time_field = name_string_t::guarantee_valid("ts");
    cfg.chunk_interval_seconds = 3600;
    return cfg;
}

}  // namespace

TPTEST(TimeSeries, ChunkedWritePathRouting) {
    /* Acceptance criteria 1+3: first insert creates chunk 0; in-order
     * inserts append (extend max_time); out-of-order inserts land in the
     * containing chunk; every chunk ends up with its own B-tree root. */
    with_ts_store([&](cache_conn_t *cache_conn, btree_slice_t *slice) {
        ql::time_series_config_t config = make_write_config();
        time_series_catalog_t catalog;
        catalog.config = config;

        /* First insert: creates chunk 0 at [1000s, 1001s). */
        EXPECT_EQ(point_write_result_t::STORED,
            routed_insert(cache_conn, slice, config, &catalog,
                          TIME_SERIES_CHUNK_TARGET_ROWS,
                          store_key_t("a"), ts_doc("a", 1000.0)));
        ASSERT_EQ(1u, catalog.chunk_index.chunks.size());
        EXPECT_EQ(1000000000ULL, catalog.chunk_index.chunks[0].min_time_us);
        EXPECT_EQ(1000000001ULL, catalog.chunk_index.chunks[0].max_time_us);
        EXPECT_EQ(1u, catalog.chunk_index.chunks[0].row_count);
        EXPECT_NE(NULL_BLOCK_ID, catalog.chunk_index.chunks[0].root_block);

        /* In-order append: same chunk, max_time extended. */
        routed_insert(cache_conn, slice, config, &catalog,
                      TIME_SERIES_CHUNK_TARGET_ROWS,
                      store_key_t("b"), ts_doc("b", 2000.0));
        ASSERT_EQ(1u, catalog.chunk_index.chunks.size());
        EXPECT_EQ(2000000001ULL, catalog.chunk_index.chunks[0].max_time_us);
        EXPECT_EQ(2u, catalog.chunk_index.chunks[0].row_count);

        /* Out-of-order within the chunk: still chunk 0, row count grows. */
        routed_insert(cache_conn, slice, config, &catalog,
                      TIME_SERIES_CHUNK_TARGET_ROWS,
                      store_key_t("c"), ts_doc("c", 1500.0));
        ASSERT_EQ(1u, catalog.chunk_index.chunks.size());
        EXPECT_EQ(3u, catalog.chunk_index.chunks[0].row_count);
        EXPECT_EQ(2000000001ULL, catalog.chunk_index.chunks[0].max_time_us);

        /* Replace on the same primary key (same chunk): row count stable. */
        routed_insert(cache_conn, slice, config, &catalog,
                      TIME_SERIES_CHUNK_TARGET_ROWS,
                      store_key_t("a"), ts_doc("a", 1700.0));
        ASSERT_EQ(1u, catalog.chunk_index.chunks.size());
        EXPECT_EQ(3u, catalog.chunk_index.chunks[0].row_count);

        /* Everything must be durable: reload in a fresh txn. */
        time_series_catalog_t reloaded = load_ts_catalog(cache_conn);
        EXPECT_TRUE(reloaded.config == config);
        EXPECT_TRUE(reloaded.chunk_index == catalog.chunk_index);
        EXPECT_NE(NULL_BLOCK_ID,
                  reloaded.chunk_index.chunks[0].root_block);
    });
}

TPTEST(TimeSeries, ChunkSealStartsNewChunk) {
    /* Acceptance criterion 7: when the newest chunk's row_count reaches the
     * threshold, the next insert seals it and starts a new chunk. */
    with_ts_store([&](cache_conn_t *cache_conn, btree_slice_t *slice) {
        ql::time_series_config_t config = make_write_config();
        time_series_catalog_t catalog;
        catalog.config = config;
        const uint64_t threshold = 2;

        routed_insert(cache_conn, slice, config, &catalog, threshold,
                      store_key_t("a"), ts_doc("a", 1000.0));
        routed_insert(cache_conn, slice, config, &catalog, threshold,
                      store_key_t("b"), ts_doc("b", 2000.0));
        ASSERT_EQ(1u, catalog.chunk_index.chunks.size());
        EXPECT_EQ(2u, catalog.chunk_index.chunks[0].row_count);

        /* Third row exceeds the threshold → chunk 1 starts at its ts. */
        routed_insert(cache_conn, slice, config, &catalog, threshold,
                      store_key_t("c"), ts_doc("c", 3000.0));
        ASSERT_EQ(2u, catalog.chunk_index.chunks.size());
        EXPECT_EQ(2u, catalog.chunk_index.chunks[0].row_count);
        EXPECT_EQ(3000000000ULL, catalog.chunk_index.chunks[1].min_time_us);
        EXPECT_EQ(1u, catalog.chunk_index.chunks[1].row_count);
        EXPECT_NE(NULL_BLOCK_ID, catalog.chunk_index.chunks[1].root_block);
        EXPECT_NE(catalog.chunk_index.chunks[0].root_block,
                  catalog.chunk_index.chunks[1].root_block);

        /* Appends after the seal extend the NEW chunk. */
        routed_insert(cache_conn, slice, config, &catalog, threshold,
                      store_key_t("d"), ts_doc("d", 3500.0));
        ASSERT_EQ(2u, catalog.chunk_index.chunks.size());
        EXPECT_EQ(3500000001ULL, catalog.chunk_index.chunks[1].max_time_us);
        EXPECT_EQ(2u, catalog.chunk_index.chunks[1].row_count);

        /* Backfill into the sealed chunk still works (containing chunk). */
        routed_insert(cache_conn, slice, config, &catalog, threshold,
                      store_key_t("e"), ts_doc("e", 1500.0));
        ASSERT_EQ(2u, catalog.chunk_index.chunks.size());
        EXPECT_EQ(3u, catalog.chunk_index.chunks[0].row_count);

        /* Older than every chunk → prepend a new chunk. */
        routed_insert(cache_conn, slice, config, &catalog, threshold,
                      store_key_t("f"), ts_doc("f", 500.0));
        ASSERT_EQ(3u, catalog.chunk_index.chunks.size());
        EXPECT_EQ(500000000ULL, catalog.chunk_index.chunks[0].min_time_us);
        EXPECT_EQ(1u, catalog.chunk_index.chunks[0].row_count);
        EXPECT_EQ(2u, catalog.chunk_index.chunks[2].row_count);

        /* Durability: fresh txn sees the sealed index. */
        time_series_catalog_t reloaded = load_ts_catalog(cache_conn);
        EXPECT_EQ(3u, reloaded.chunk_index.chunks.size());
        EXPECT_TRUE(reloaded.chunk_index == catalog.chunk_index);
        EXPECT_EQ(6u, time_series_ops_t::total_rows(reloaded.chunk_index));
    });
}

TPTEST(TimeSeries, ChunkIntervalSealsNewChunk) {
    /* A row at/after the chunk's interval boundary starts a new chunk even
     * when the row-count threshold was not reached. */
    with_ts_store([&](cache_conn_t *cache_conn, btree_slice_t *slice) {
        ql::time_series_config_t config = make_write_config();
        time_series_catalog_t catalog;
        catalog.config = config;

        routed_insert(cache_conn, slice, config, &catalog,
                      TIME_SERIES_CHUNK_TARGET_ROWS,
                      store_key_t("a"), ts_doc("a", 1000.0));
        /* 1000s + 3600s interval boundary = 4600s; at the boundary → new. */
        routed_insert(cache_conn, slice, config, &catalog,
                      TIME_SERIES_CHUNK_TARGET_ROWS,
                      store_key_t("b"), ts_doc("b", 4600.0));
        ASSERT_EQ(2u, catalog.chunk_index.chunks.size());
        EXPECT_EQ(4600000000ULL, catalog.chunk_index.chunks[1].min_time_us);
        /* Just below the boundary stays in chunk 0. */
        routed_insert(cache_conn, slice, config, &catalog,
                      TIME_SERIES_CHUNK_TARGET_ROWS,
                      store_key_t("c"), ts_doc("c", 4599.0));
        ASSERT_EQ(2u, catalog.chunk_index.chunks.size());
        EXPECT_EQ(2u, catalog.chunk_index.chunks[0].row_count);
    });
}

TPTEST(TimeSeries, ChunkedWriteRejectsBadTimeField) {
    /* Acceptance criteria 4+5: missing field and non-time field are
     * rejected with the catalog errors, before any chunk mutation. */
    with_ts_store([&](cache_conn_t *cache_conn, btree_slice_t *slice) {
        ql::time_series_config_t config = make_write_config();
        time_series_catalog_t catalog;
        catalog.config = config;

        /* Missing field. */
        ql::datum_object_builder_t no_ts;
        no_ts.overwrite("id", ql::datum_t(datum_string_t("x")));
        expect_reql_error("TIME_SERIES_FIELD_MISSING", [&] {
            routed_insert(cache_conn, slice, config, &catalog,
                          TIME_SERIES_CHUNK_TARGET_ROWS,
                          store_key_t("x"),
                          std::move(no_ts).to_datum());
        });

        /* Non-object document. */
        expect_reql_error("TIME_SERIES_FIELD_MISSING", [&] {
            routed_insert(cache_conn, slice, config, &catalog,
                          TIME_SERIES_CHUNK_TARGET_ROWS,
                          store_key_t("n"), ql::datum_t(42.0));
        });

        /* Wrong type (string instead of a TIME pseudo-type). */
        ql::datum_object_builder_t bad_ts;
        bad_ts.overwrite("id", ql::datum_t(datum_string_t("y")));
        bad_ts.overwrite("ts", ql::datum_t(datum_string_t("not-a-time")));
        expect_reql_error("TIME_SERIES_FIELD_INVALID_TYPE", [&] {
            routed_insert(cache_conn, slice, config, &catalog,
                          TIME_SERIES_CHUNK_TARGET_ROWS,
                          store_key_t("y"),
                          std::move(bad_ts).to_datum());
        });

        /* Nothing was routed: no chunks exist. */
        EXPECT_TRUE(catalog.chunk_index.chunks.empty());
        EXPECT_EQ(0u, time_series_ops_t::total_rows(catalog.chunk_index));
    });
}

TPTEST(TimeSeries, ChunkedWriteIntervalAndRows) {
    /* TIME_SERIES_CHUNK_TARGET_ROWS is a sane constant, and total_rows
     * aggregates across chunks. */
    EXPECT_GE(TIME_SERIES_CHUNK_TARGET_ROWS, 1000u);
    ql::time_chunk_index_t idx;
    idx.chunks.push_back(ql::time_chunk_bounds_t());
    idx.chunks.push_back(ql::time_chunk_bounds_t());
    idx.chunks[0].row_count = 5;
    idx.chunks[1].row_count = 7;
    EXPECT_EQ(12u, time_series_ops_t::total_rows(idx));
}

TEST(TimeSeries, ChunkInfoDatumShape) {
    /* The config() chunk-info datum must expose chunk_count, total_rows,
     * newest bounds and the chunk list; null when no catalog exists. */
    EXPECT_EQ(ql::datum_t::null(),
        ql::format_time_series_chunk_info_datum(
            ql::time_chunk_index_t(), false));

    ql::time_chunk_index_t idx;
    idx.chunks.push_back(ql::time_chunk_bounds_t());
    idx.chunks[0].min_time_us = 1000000000ULL;
    idx.chunks[0].max_time_us = 2000000001ULL;
    idx.chunks[0].row_count = 3;
    ql::datum_t d = ql::format_time_series_chunk_info_datum(idx, true);
    EXPECT_EQ(1.0, d.get_field("chunk_count").as_num());
    EXPECT_EQ(3.0, d.get_field("total_rows").as_num());
    ql::datum_t newest = d.get_field("newest");
    EXPECT_EQ(1000000000.0, newest.get_field("min_time_us").as_num());
    EXPECT_EQ(2000000001.0, newest.get_field("max_time_us").as_num());
    EXPECT_EQ(3.0, newest.get_field("row_count").as_num());
    EXPECT_EQ(1u, d.get_field("chunks").arr_size());
}

TPTEST(TimeSeries, ChunkedCatalogRelease) {
    /* release_catalog must free the blob and reset the superblock slot. */
    with_ts_store([&](cache_conn_t *cache_conn, btree_slice_t *slice) {
        time_series_catalog_t catalog;
        catalog.config = make_write_config();
        {
            scoped_ptr_t<txn_t> txn;
            scoped_ptr_t<real_superblock_t> superblock;
            get_btree_superblock_and_txn_for_writing(
                cache_conn, nullptr, write_access_t::write, 4,
                write_durability_t::SOFT, &superblock, &txn);
            time_series_ops_t::save_catalog(superblock.get(), catalog);
            EXPECT_NE(NULL_BLOCK_ID,
                      superblock->get_time_series_catalog_block_id());
            time_series_ops_t::release_catalog(superblock.get());
            EXPECT_EQ(NULL_BLOCK_ID,
                      superblock->get_time_series_catalog_block_id());
            superblock.reset();
            txn->commit();
        }
        /* After release, load returns a default catalog. */
        time_series_catalog_t reloaded = load_ts_catalog(cache_conn);
        EXPECT_FALSE(reloaded.config.enabled);
        EXPECT_TRUE(reloaded.chunk_index.chunks.empty());
    });
}

TPTEST(TimeSeries, ChunkedCatalogPersistsAcrossTxns) {
    /* Live-path pattern: every write RELOADS the catalog from disk (a fresh
     * txn), routes into it, and saves. This must accumulate chunks across
     * transactions — a fresh load must see the previous save. Mirrors
     * chunked_batched_insert() in store.cc. */
    with_ts_store([&](cache_conn_t *cache_conn, btree_slice_t *slice) {
        ql::time_series_config_t config = make_write_config();
        const uint64_t threshold = TIME_SERIES_CHUNK_TARGET_ROWS;

        /* Write 1: fresh catalog (no block id yet) → creates block + chunk 0. */
        {
            time_series_catalog_t catalog =
                load_ts_catalog(cache_conn);
            catalog.config = config;
            point_write_response_t pw_res;
            rdb_modification_info_t mod_info;
            rdb_live_deletion_context_t deletion_context;
            scoped_ptr_t<txn_t> txn;
            scoped_ptr_t<real_superblock_t> superblock;
            get_btree_superblock_and_txn_for_writing(
                cache_conn, nullptr, write_access_t::write, 4,
                write_durability_t::SOFT, &superblock, &txn);
            time_series_ops_t::route_insert(
                slice, superblock.get(), config, &catalog, threshold,
                store_key_t("a"), ts_doc("a", 1000.0), true,
                repli_timestamp_t::distant_past, &deletion_context,
                &pw_res, &mod_info, nullptr);
            time_series_ops_t::save_catalog(superblock.get(), catalog);
            superblock.reset();
            txn->commit();
        }

        /* Write 2: must see write 1's chunk (block id + blob persisted). */
        {
            time_series_catalog_t catalog =
                load_ts_catalog(cache_conn);
            EXPECT_EQ(1u, catalog.chunk_index.chunks.size())
                << "write 2 must load write 1's catalog (block id persisted)";
            EXPECT_EQ(1u, catalog.chunk_index.chunks[0].row_count);
            catalog.config = config;
            point_write_response_t pw_res;
            rdb_modification_info_t mod_info;
            rdb_live_deletion_context_t deletion_context;
            scoped_ptr_t<txn_t> txn;
            scoped_ptr_t<real_superblock_t> superblock;
            get_btree_superblock_and_txn_for_writing(
                cache_conn, nullptr, write_access_t::write, 4,
                write_durability_t::SOFT, &superblock, &txn);
            time_series_ops_t::route_insert(
                slice, superblock.get(), config, &catalog, threshold,
                store_key_t("b"), ts_doc("b", 2000.0), true,
                repli_timestamp_t::distant_past, &deletion_context,
                &pw_res, &mod_info, nullptr);
            time_series_ops_t::save_catalog(superblock.get(), catalog);
            superblock.reset();
            txn->commit();
        }

        /* Write 3: must see 2 chunks' worth of rows (1 chunk, 2 rows). */
        {
            time_series_catalog_t catalog =
                load_ts_catalog(cache_conn);
            EXPECT_EQ(1u, catalog.chunk_index.chunks.size())
                << "write 3 must load write 2's catalog";
            EXPECT_EQ(2u, catalog.chunk_index.chunks[0].row_count)
                << "rows must accumulate across transactions";
            EXPECT_EQ(2000000001ULL, catalog.chunk_index.chunks[0].max_time_us);
        }
    });
}

TPTEST(TimeSeries, LiveVisitorSequenceWithSindexBlock) {
    /* Mirrors the LIVE server write path (store.cc chunked_batched_insert +
     * store_t::write): acquire the sindex block parented on the superblock
     * BEFORE the route+save, release it via reset_buf_lock() (as
     * update_sindexes does), then commit. The catalog block id must survive
     * the commit — regression probe for the live-server field loss. */
    with_ts_store([&](cache_conn_t *cache_conn, btree_slice_t *slice) {
        ql::time_series_config_t config = make_write_config();

        {
            scoped_ptr_t<txn_t> txn;
            scoped_ptr_t<real_superblock_t> superblock;
            get_btree_superblock_and_txn_for_writing(
                cache_conn, nullptr, write_access_t::write, 4,
                write_durability_t::SOFT, &superblock, &txn);
            /* sindex block acquisition exactly like rdb_write_visitor_t ctor */
            buf_lock_t sindex_block(superblock->expose_buf(),
                                    superblock->get_sindex_block_id(),
                                    access_t::write);
            time_series_catalog_t catalog =
                time_series_ops_t::load_catalog(superblock.get());
            catalog.config = config;
            point_write_response_t pw_res;
            rdb_modification_info_t mod_info;
            rdb_live_deletion_context_t deletion_context;
            time_series_ops_t::route_insert(
                slice, superblock.get(), config, &catalog,
                TIME_SERIES_CHUNK_TARGET_ROWS,
                store_key_t("a"), ts_doc("a", 1000.0), true,
                repli_timestamp_t::distant_past, &deletion_context,
                &pw_res, &mod_info, nullptr);
            time_series_ops_t::save_catalog(superblock.get(), catalog);
            EXPECT_NE(NULL_BLOCK_ID,
                      superblock->get_time_series_catalog_block_id())
                << "field must be set before commit";
            /* update_sindexes releases the sindex block like this */
            sindex_block.reset_buf_lock();
            superblock.reset();
            txn->commit();
        }

        /* Fresh txn must see the catalog block id (live server loses it). */
        time_series_catalog_t reloaded = load_ts_catalog(cache_conn);
        EXPECT_EQ(1u, reloaded.chunk_index.chunks.size())
            << "catalog block id must survive commit (live-server regression)";
        EXPECT_EQ(1u, reloaded.chunk_index.chunks[0].row_count);
        EXPECT_NE(NULL_BLOCK_ID, reloaded.chunk_index.chunks[0].root_block);
    });
}

/* ── PHASE3-TS-4: retention + compaction (spec §5.2/§6.4/§7/§8.1) ─────── */

TEST(TimeSeries, RetentionExpiredChunksBoundary) {
    /* Strict-less-than TTL semantics: a chunk is expired only when its
     * newest row (max_time_us, exclusive) is strictly older than the
     * cutoff; exactly-at-TTL chunks are kept (§8.1). */
    ql::time_chunk_index_t idx;
    /* Three adjacent chunks: [0,100s), [100s,200s), [200s,300s). */
    idx.chunks.push_back(ql::time_chunk_bounds_t{0, 100000000ULL, 10});
    idx.chunks.push_back(ql::time_chunk_bounds_t{100000000ULL,
                                                 200000000ULL, 20});
    idx.chunks.push_back(ql::time_chunk_bounds_t{200000000ULL,
                                                 300000000ULL, 30});
    const uint64_t now_us = 300000000ULL;  // 300s

    /* retention 150s → cutoff 150s: chunk0 (max 100s) expired; chunk1
     * (max 200s) kept. */
    EXPECT_EQ(std::vector<size_t>({0}),
        time_series_ops_t::expired_chunks(idx, now_us, 150));

    /* retention 100s → cutoff 200s: chunk1's max == cutoff exactly →
     * kept (strict <). Chunk0 still expired. */
    EXPECT_EQ(std::vector<size_t>({0}),
        time_series_ops_t::expired_chunks(idx, now_us, 100));

    /* retention 90s → cutoff 210s: chunks 0 and 1 expired. */
    EXPECT_EQ(std::vector<size_t>({0, 1}),
        time_series_ops_t::expired_chunks(idx, now_us, 90));

    /* retention 200s → cutoff 100s: chunk0's max == cutoff exactly →
     * kept; nothing expired. */
    EXPECT_EQ(std::vector<size_t>({}),
        time_series_ops_t::expired_chunks(idx, now_us, 200));

    /* retention 300s → cutoff 0: nothing expired (all maxes >= cutoff). */
    EXPECT_EQ(std::vector<size_t>({}),
        time_series_ops_t::expired_chunks(idx, now_us, 300));
}

TEST(TimeSeries, RetentionDisabledAndClockUnderflow) {
    ql::time_chunk_index_t idx;
    idx.chunks.push_back(ql::time_chunk_bounds_t{0, 100000000ULL, 10});
    idx.chunks.push_back(ql::time_chunk_bounds_t{100000000ULL,
                                                 200000000ULL, 20});

    /* retention = 0 disables retention (§5.2): nothing ever expires. */
    EXPECT_EQ(std::vector<size_t>({}),
        time_series_ops_t::expired_chunks(idx, 1000000000000ULL, 0));

    /* Clock underflow: now (50s) < retention (100s) → cutoff clamps to 0
     * and nothing can be strictly below it. */
    EXPECT_EQ(std::vector<size_t>({}),
        time_series_ops_t::expired_chunks(idx, 50000000ULL, 100));

    /* Empty index → empty result. */
    ql::time_chunk_index_t empty;
    EXPECT_EQ(std::vector<size_t>({}),
        time_series_ops_t::expired_chunks(empty, 1000000000000ULL, 10));
}

TEST(TimeSeries, CompactibleChunksSelection) {
    ql::time_chunk_index_t idx;
    idx.chunks.push_back(ql::time_chunk_bounds_t{0, 100000000ULL, 10});
    idx.chunks.push_back(ql::time_chunk_bounds_t{100000000ULL,
                                                 200000000ULL, 0});
    idx.chunks.push_back(ql::time_chunk_bounds_t{200000000ULL,
                                                 300000000ULL, 5});
    const uint64_t now_us = 300000000ULL;  // 300s

    /* min age 150s → cutoff 150s: chunk0 (max 100s) compactible; chunk1
     * has no rows; chunk2 is the newest (active) chunk → never. */
    EXPECT_EQ(std::vector<size_t>({0}),
        time_series_ops_t::compactible_chunks(idx, now_us, 150));

    /* min age 100s → cutoff 200s: chunk0 only (chunk1 empty). */
    EXPECT_EQ(std::vector<size_t>({0}),
        time_series_ops_t::compactible_chunks(idx, now_us, 100));

    /* min age 0 → cutoff now: every sealed chunk with rows. */
    EXPECT_EQ(std::vector<size_t>({0}),
        time_series_ops_t::compactible_chunks(idx, now_us, 0));

    /* min age 300s → cutoff 0 (underflow-safe): nothing. */
    EXPECT_EQ(std::vector<size_t>({}),
        time_series_ops_t::compactible_chunks(idx, now_us, 300));

    /* A single-chunk index has no sealed chunk at all. */
    ql::time_chunk_index_t single;
    single.chunks.push_back(ql::time_chunk_bounds_t{0, 100000000ULL, 10});
    EXPECT_EQ(std::vector<size_t>({}),
        time_series_ops_t::compactible_chunks(single, now_us, 0));
}

namespace {

/* Runs `fn` with a write superblock in its own transaction (the jobs
 * pattern: one bounded transaction per chunk). If `fn` throws (e.g. a
 * corrupt-chunk rejection), the empty transaction is committed before
 * rethrowing — aborting a write txn is process death by design. */
template <class F>
void with_write_superblock(cache_conn_t *cache_conn, F &&fn) {
    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_t> superblock;
    get_btree_superblock_and_txn_for_writing(
        cache_conn, nullptr, write_access_t::write, 4,
        write_durability_t::SOFT, &superblock, &txn);
    try {
        fn(superblock.get());
    } catch (...) {
        /* Error paths reject before any chunk mutation; commit the empty
         * txn to release the superblock cleanly. */
        superblock.reset();
        txn->commit();
        throw;
    }
    superblock.reset();
    txn->commit();
}

/* Counts rows in a chunk tree (for verifying erasure / rewrite). */
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

size_t count_chunk_rows(cache_conn_t *cache_conn, block_id_t root) {
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

/* Collects every (key, datum) pair of a sub-tree (for verifying the
 * downsample tree's bucket rows). */
class tree_collect_visitor_t : public depth_first_traversal_callback_t {
public:
    continue_bool_t handle_pair(
            scoped_key_value_t &&keyvalue,
            UNUSED signal_t *interruptor) override {
        const store_key_t key(keyvalue.key());
        const rdb_value_t *value =
            static_cast<const rdb_value_t *>(keyvalue.value());
        rows.emplace_back(key, get_data(value, keyvalue.expose_buf()));
        return continue_bool_t::CONTINUE;
    }

    std::vector<std::pair<store_key_t, ql::datum_t>> rows;
};

std::vector<std::pair<store_key_t, ql::datum_t>> collect_tree_rows(
        cache_conn_t *cache_conn, block_id_t root) {
    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_t> superblock;
    get_btree_superblock_and_txn_for_reading(
        cache_conn, CACHE_SNAPSHOTTED_NO, &superblock, &txn);
    time_chunk_superblock_t read_sb(
        superblock.get(), root, superblock->get_stat_block_id());
    tree_collect_visitor_t visitor;
    cond_t interruptor;
    btree_depth_first_traversal(
        &read_sb, key_range_t::universe(), &visitor,
        access_t::read, direction_t::FORWARD,
        release_superblock_t::KEEP, &interruptor);
    return std::move(visitor.rows);
}

}  // namespace

TPTEST(TimeSeries, RetentionExpireChunkRemovesFromIndex) {
    /* expire_chunk erases the chunk's tree, removes the chunk from the
     * index, and the caller's catalog save makes it durable. */
    with_ts_store([&](cache_conn_t *cache_conn, btree_slice_t *slice) {
        ql::time_series_config_t config = make_write_config();
        time_series_catalog_t catalog;
        catalog.config = config;
        const uint64_t threshold = 2;
        for (int i = 0; i < 6; ++i) {
            const std::string key = strprintf("k%d", i);
            routed_insert(cache_conn, slice, config, &catalog, threshold,
                          store_key_t(key.c_str()),
                          ts_doc(key.c_str(), 1000.0 + i));
        }
        ASSERT_EQ(3u, catalog.chunk_index.chunks.size());
        ASSERT_EQ(6u, time_series_ops_t::total_rows(catalog.chunk_index));
        const block_id_t remaining_roots[] = {
            catalog.chunk_index.chunks[1].root_block,
            catalog.chunk_index.chunks[2].root_block};

        std::vector<rdb_modification_report_t> mod_reports;
        rdb_live_deletion_context_t deletion_context;
        cond_t non_interruptor;
        uint64_t freed = 0;
        with_write_superblock(cache_conn, [&](real_superblock_t *superblock) {
            freed = time_series_ops_t::expire_chunk(
                slice, superblock, &catalog, 0, &deletion_context,
                &non_interruptor, &mod_reports);
            time_series_ops_t::save_catalog(superblock, catalog);
        });
        EXPECT_EQ(2u, freed);
        EXPECT_EQ(2u, mod_reports.size());
        EXPECT_EQ(2u, catalog.chunk_index.chunks.size());
        EXPECT_EQ(4u, time_series_ops_t::total_rows(catalog.chunk_index));

        /* Durability: fresh txn sees the shrunken index. */
        time_series_catalog_t reloaded = load_ts_catalog(cache_conn);
        EXPECT_EQ(2u, reloaded.chunk_index.chunks.size());
        EXPECT_EQ(4u, reloaded.chunk_index.chunks[0].row_count
                      + reloaded.chunk_index.chunks[1].row_count);
        EXPECT_TRUE(reloaded.chunk_index == catalog.chunk_index);

        /* The surviving chunk trees still hold their rows. */
        EXPECT_EQ(2u, count_chunk_rows(cache_conn, remaining_roots[0]));
        EXPECT_EQ(2u, count_chunk_rows(cache_conn, remaining_roots[1]));
    });
}

TPTEST(TimeSeries, RetentionResumesFromCheckpoint) {
    /* Crash-safety: each chunk's erase + index removal commit together,
     * so the durable chunk index is the checkpoint. Simulate a crash
     * between chunks by reloading the catalog in a fresh transaction and
     * re-running the pass: the expired chunk already committed is gone,
     * and the next pass resumes from the front of the index. */
    with_ts_store([&](cache_conn_t *cache_conn, btree_slice_t *slice) {
        ql::time_series_config_t config = make_write_config();
        time_series_catalog_t catalog;
        catalog.config = config;
        const uint64_t threshold = 2;
        for (int i = 0; i < 5; ++i) {
            const std::string key = strprintf("k%d", i);
            routed_insert(cache_conn, slice, config, &catalog, threshold,
                          store_key_t(key.c_str()),
                          ts_doc(key.c_str(), 1000.0 + i));
        }
        routed_insert(cache_conn, slice, config, &catalog, threshold,
                      store_key_t("k5"), ts_doc("k5", 2900.0));
        ASSERT_EQ(3u, catalog.chunk_index.chunks.size());
        ASSERT_EQ(6u, time_series_ops_t::total_rows(catalog.chunk_index));

        const uint64_t now_us = 3000000000ULL;  // 3000s
        const uint64_t retention = 150;          // cutoff 2850s
        EXPECT_EQ(std::vector<size_t>({0, 1}),
            time_series_ops_t::expired_chunks(catalog.chunk_index,
                                              now_us, retention));

        /* Pass 1: expire chunk 0, commit (the "tick" completes). */
        rdb_live_deletion_context_t deletion_context;
        cond_t non_interruptor;
        with_write_superblock(cache_conn, [&](real_superblock_t *superblock) {
            std::vector<rdb_modification_report_t> mod_reports;
            time_series_ops_t::expire_chunk(
                slice, superblock, &catalog, 0, &deletion_context,
                &non_interruptor, &mod_reports);
            time_series_ops_t::save_catalog(superblock, catalog);
        });

        /* "Crash": the durable index is the checkpoint. */
        time_series_catalog_t reloaded = load_ts_catalog(cache_conn);
        EXPECT_EQ(2u, reloaded.chunk_index.chunks.size());
        EXPECT_EQ(4u, time_series_ops_t::total_rows(reloaded.chunk_index));
        EXPECT_EQ(1002000000ULL, reloaded.chunk_index.chunks[0].min_time_us)
            << "committed expiry must not reappear after the reload";

        /* Resume: the next tick finds the remaining expired chunk at the
         * front of the (consistent) index. */
        EXPECT_EQ(std::vector<size_t>({0}),
            time_series_ops_t::expired_chunks(reloaded.chunk_index,
                                              now_us, retention));

        /* Pass 2: expire it; the live chunk (max 2901s) survives. */
        with_write_superblock(cache_conn, [&](real_superblock_t *superblock) {
            std::vector<rdb_modification_report_t> mod_reports;
            time_series_ops_t::expire_chunk(
                slice, superblock, &reloaded, 0, &deletion_context,
                &non_interruptor, &mod_reports);
            time_series_ops_t::save_catalog(superblock, reloaded);
        });
        time_series_catalog_t final_catalog = load_ts_catalog(cache_conn);
        EXPECT_EQ(1u, final_catalog.chunk_index.chunks.size());
        EXPECT_EQ(2u, time_series_ops_t::total_rows(final_catalog.chunk_index));
        /* The surviving chunk was created at 1004s and extended by the
         * 2900s row: its min is the chunk's creation time, not the row's. */
        EXPECT_EQ(1004000000ULL,
                  final_catalog.chunk_index.chunks[0].min_time_us);
        EXPECT_EQ(std::vector<size_t>({}),
            time_series_ops_t::expired_chunks(final_catalog.chunk_index,
                                              now_us, retention));
    });
}

TPTEST(TimeSeries, RetentionCorruptChunkAborts) {
    /* §7: a chunk index entry that claims rows but has no tree root is
     * corrupt; expire_chunk raises TIME_SERIES_CHUNK_CORRUPT BEFORE any
     * modification — no erase, no index change, no reports. */
    with_ts_store([&](cache_conn_t *cache_conn, btree_slice_t *slice) {
        ql::time_series_config_t config = make_write_config();
        time_series_catalog_t catalog;
        catalog.config = config;
        routed_insert(cache_conn, slice, config, &catalog,
                      TIME_SERIES_CHUNK_TARGET_ROWS,
                      store_key_t("a"), ts_doc("a", 1000.0));
        routed_insert(cache_conn, slice, config, &catalog,
                      TIME_SERIES_CHUNK_TARGET_ROWS,
                      store_key_t("b"), ts_doc("b", 2000.0));
        ASSERT_EQ(1u, catalog.chunk_index.chunks.size());
        const block_id_t real_root =
            catalog.chunk_index.chunks[0].root_block;
        EXPECT_FALSE(time_series_ops_t::chunk_is_corrupt(
            catalog.chunk_index.chunks[0]));

        /* Corrupt the index: claim the rows but drop the root. */
        catalog.chunk_index.chunks[0].root_block = NULL_BLOCK_ID;
        EXPECT_TRUE(time_series_ops_t::chunk_is_corrupt(
            catalog.chunk_index.chunks[0]));

        std::vector<rdb_modification_report_t> mod_reports;
        rdb_live_deletion_context_t deletion_context;
        cond_t non_interruptor;
        expect_reql_error("TIME_SERIES_CHUNK_CORRUPT", [&] {
            with_write_superblock(cache_conn, [&](real_superblock_t *superblock) {
                time_series_ops_t::expire_chunk(
                    slice, superblock, &catalog, 0, &deletion_context,
                    &non_interruptor, &mod_reports);
            });
        });

        /* Nothing was modified: chunk still in the index, no reports. */
        EXPECT_EQ(1u, catalog.chunk_index.chunks.size());
        EXPECT_EQ(2u, catalog.chunk_index.chunks[0].row_count);
        EXPECT_TRUE(mod_reports.empty());

        /* The underlying data is untouched (the raise happened before the
         * erase). */
        EXPECT_EQ(2u, count_chunk_rows(cache_conn, real_root));
    });
}

TPTEST(TimeSeries, CompactionRewritesSealedChunk) {
    /* compact_chunk rebuilds a sealed chunk's tree in one transaction:
     * every row survives, the root block is replaced, and the rewrite is
     * durable. */
    with_ts_store([&](cache_conn_t *cache_conn, btree_slice_t *slice) {
        ql::time_series_config_t config = make_write_config();
        time_series_catalog_t catalog;
        catalog.config = config;
        const uint64_t threshold = 2;
        for (int i = 0; i < 4; ++i) {
            const std::string key = strprintf("k%d", i);
            routed_insert(cache_conn, slice, config, &catalog, threshold,
                          store_key_t(key.c_str()),
                          ts_doc(key.c_str(), 1000.0 + i));
        }
        routed_insert(cache_conn, slice, config, &catalog, threshold,
                      store_key_t("k4"), ts_doc("k4", 3000.0));
        ASSERT_EQ(3u, catalog.chunk_index.chunks.size());
        ASSERT_EQ(2u, catalog.chunk_index.chunks[0].row_count);
        const block_id_t old_root =
            catalog.chunk_index.chunks[0].root_block;

        rdb_live_deletion_context_t deletion_context;
        cond_t non_interruptor;
        uint64_t rows = 0;
        with_write_superblock(cache_conn, [&](real_superblock_t *superblock) {
            rows = time_series_ops_t::compact_chunk(
                slice, superblock, &catalog, 0, &deletion_context,
                &non_interruptor);
            time_series_ops_t::save_catalog(superblock, catalog);
        });
        EXPECT_EQ(2u, rows);
        EXPECT_NE(old_root, catalog.chunk_index.chunks[0].root_block)
            << "compaction must publish a fresh root block";
        EXPECT_EQ(2u, catalog.chunk_index.chunks[0].row_count);
        EXPECT_EQ(5u, time_series_ops_t::total_rows(catalog.chunk_index));

        /* Durability + row preservation: fresh txn sees the new root, and
         * the rewritten tree holds all rows. */
        time_series_catalog_t reloaded = load_ts_catalog(cache_conn);
        EXPECT_EQ(3u, reloaded.chunk_index.chunks.size());
        EXPECT_EQ(catalog.chunk_index.chunks[0].root_block,
                  reloaded.chunk_index.chunks[0].root_block);
        EXPECT_EQ(2u, count_chunk_rows(
            cache_conn, reloaded.chunk_index.chunks[0].root_block));
    });
}

TEST(TimeSeries, RetentionOnlyReconfigureAllowed) {
    /* Spec §6.1: retention is the one mutable time-series option. The
     * helper accepts identical datums and retention-only changes, and
     * rejects every other mutation. */
    ql::time_series_config_t cfg = make_ts_config();
    cfg.downsample_steps.clear();
    const ql::datum_t base = ql::format_time_series_config_datum(cfg);

    /* Identical datum → allowed. */
    EXPECT_TRUE(ql::time_series_reconfigure_allows_retention_change(
        base, base));

    /* Retention-only change → allowed. */
    ql::datum_object_builder_t new_ret;
    new_ret.overwrite("field",
        base.get_field(datum_string_t("field"), ql::NOTHROW));
    new_ret.overwrite("chunk_interval",
        base.get_field(datum_string_t("chunk_interval"), ql::NOTHROW));
    new_ret.overwrite("retention", ql::datum_t(3600.0));
    new_ret.overwrite("downsample",
        base.get_field(datum_string_t("downsample"), ql::NOTHROW));
    const ql::datum_t retention_only = std::move(new_ret).to_datum();
    EXPECT_TRUE(ql::time_series_reconfigure_allows_retention_change(
        base, retention_only));

    /* Any other field change → rejected. */
    ql::datum_object_builder_t bad_field;
    bad_field.overwrite("field", ql::datum_t(datum_string_t("other")));
    bad_field.overwrite("chunk_interval",
        base.get_field(datum_string_t("chunk_interval"), ql::NOTHROW));
    bad_field.overwrite("retention",
        base.get_field(datum_string_t("retention"), ql::NOTHROW));
    bad_field.overwrite("downsample",
        base.get_field(datum_string_t("downsample"), ql::NOTHROW));
    EXPECT_FALSE(ql::time_series_reconfigure_allows_retention_change(
        base, std::move(bad_field).to_datum()));

    ql::datum_object_builder_t bad_chunk;
    bad_chunk.overwrite("field",
        base.get_field(datum_string_t("field"), ql::NOTHROW));
    bad_chunk.overwrite("chunk_interval", ql::datum_t(60.0));
    bad_chunk.overwrite("retention",
        base.get_field(datum_string_t("retention"), ql::NOTHROW));
    bad_chunk.overwrite("downsample",
        base.get_field(datum_string_t("downsample"), ql::NOTHROW));
    EXPECT_FALSE(ql::time_series_reconfigure_allows_retention_change(
        base, std::move(bad_chunk).to_datum()));

    ql::datum_object_builder_t bad_downsample;
    bad_downsample.overwrite("field",
        base.get_field(datum_string_t("field"), ql::NOTHROW));
    bad_downsample.overwrite("chunk_interval",
        base.get_field(datum_string_t("chunk_interval"), ql::NOTHROW));
    bad_downsample.overwrite("retention",
        base.get_field(datum_string_t("retention"), ql::NOTHROW));
    ql::datum_object_builder_t agg;
    agg.overwrite("avg", ql::datum_t(datum_string_t("x")));
    ql::datum_object_builder_t step;
    step.overwrite("age", ql::datum_t(86400.0));
    step.overwrite("to", ql::datum_t(60.0));
    step.overwrite("aggregate", std::move(agg).to_datum());
    ql::datum_array_builder_t steps(ql::configured_limits_t::unlimited);
    steps.add(std::move(step).to_datum());
    bad_downsample.overwrite("downsample", std::move(steps).to_datum());
    EXPECT_FALSE(ql::time_series_reconfigure_allows_retention_change(
        base, std::move(bad_downsample).to_datum()));

    /* Missing key / non-object / null → rejected. */
    ql::datum_object_builder_t missing_ret;
    missing_ret.overwrite("field",
        base.get_field(datum_string_t("field"), ql::NOTHROW));
    missing_ret.overwrite("chunk_interval",
        base.get_field(datum_string_t("chunk_interval"), ql::NOTHROW));
    missing_ret.overwrite("downsample",
        base.get_field(datum_string_t("downsample"), ql::NOTHROW));
    EXPECT_FALSE(ql::time_series_reconfigure_allows_retention_change(
        base, std::move(missing_ret).to_datum()));
    EXPECT_FALSE(ql::time_series_reconfigure_allows_retention_change(
        base, ql::datum_t::null()));
    EXPECT_FALSE(ql::time_series_reconfigure_allows_retention_change(
        ql::datum_t::null(), base));
    EXPECT_FALSE(ql::time_series_reconfigure_allows_retention_change(
        base, ql::datum_t(42.0)));
}

/* PHASE3-TS-5 §8.2: end-to-end downsample integration — insert rows
 * spanning multiple buckets, fold the sealed chunk through the merge
 * job's per-chunk transaction (the same helper run_downsample_pass
 * calls), and verify the downsample tree holds the expected aggregate
 * rows: bucket keys plus computed aggregate values. */
TPTEST(TimeSeries, DownsampleMergeWritesAggregateRows) {
    with_ts_store([&](cache_conn_t *cache_conn, btree_slice_t *slice) {
        /* One downsample step: 2s target interval, 10s age. Aggregates
         * are real compiled ReQL funcs (minidriver), the same shape the
         * config validator accepts at table creation. */
        ql::time_series_config_t config = make_write_config();
        ql::downsample_step_t step;
        step.age_seconds = 10;
        step.target_interval_seconds = 2;
        ql::minidriver_t r(ql::backtrace_id_t::empty());
        ql::sym_t batch(0);
        step.aggregates[name_string_t::guarantee_valid("avg_temp")] =
            ql::wire_func_t(
                r.var(batch).call(Term::AVG, "temperature").root_term(),
                std::vector<ql::sym_t>{batch});
        step.aggregates[name_string_t::guarantee_valid("cnt")] =
            ql::wire_func_t(
                r.var(batch).count().root_term(),
                std::vector<ql::sym_t>{batch});
        config.downsample_steps.push_back(step);

        time_series_catalog_t catalog;
        catalog.config = config;
        const uint64_t threshold = 4;
        for (int i = 0; i < 6; ++i) {
            ql::datum_object_builder_t doc;
            doc.overwrite("id", ql::datum_t(datum_string_t(
                strprintf("k%d", i).c_str())));
            doc.overwrite("ts",
                ql::pseudo::make_time(1000.0 + i, "+00:00"));
            doc.overwrite("temperature", ql::datum_t(10.0 * i));
            routed_insert(cache_conn, slice, config, &catalog, threshold,
                          store_key_t(strprintf("k%d", i).c_str()),
                          std::move(doc).to_datum());
        }
        /* k0..k3 → chunk 0 (sealed by k4); k4,k5 → newest chunk 1. */
        ASSERT_EQ(2u, catalog.chunk_index.chunks.size());
        ASSERT_EQ(4u, catalog.chunk_index.chunks[0].row_count);
        ASSERT_EQ(2u, catalog.chunk_index.chunks[1].row_count);
        EXPECT_FALSE(catalog.chunk_index.chunks[0].merged);
        /* The job's candidate selection sees exactly the sealed chunk. */
        EXPECT_EQ(std::vector<size_t>({0}),
                  time_series_ops_t::downsample_candidates(
                      catalog.chunk_index));

        /* The merge pass: fold chunk 0 into the step inside one write
         * transaction, exactly as run_downsample_pass does. */
        rdb_live_deletion_context_t deletion_context;
        cond_t non_interruptor;
        uint64_t written = 0;
        with_write_superblock(cache_conn, [&](real_superblock_t *sb) {
            written = time_series_ops_t::merge_chunk_into_downsample_steps(
                slice, sb, &catalog, 0, &deletion_context,
                &non_interruptor);
            time_series_ops_t::save_catalog(sb, catalog);
        });

        /* 4 rows at ts 1000..1003s → two 2s buckets: [1000s,1002s) and
         * [1002s,1004s). The step's tree is created and holds one
         * aggregate row per bucket. */
        EXPECT_EQ(2u, written);
        EXPECT_TRUE(catalog.chunk_index.chunks[0].merged);
        EXPECT_FALSE(catalog.chunk_index.chunks[1].merged)
            << "the active newest chunk is never merged";
        EXPECT_TRUE(time_series_ops_t::downsample_candidates(
            catalog.chunk_index).empty());
        ASSERT_EQ(1u, catalog.downsample_roots.size());
        ASSERT_NE(NULL_BLOCK_ID, catalog.downsample_roots[0].root_block);

        std::vector<std::pair<store_key_t, ql::datum_t>> agg_rows =
            collect_tree_rows(cache_conn,
                              catalog.downsample_roots[0].root_block);
        ASSERT_EQ(2u, agg_rows.size());
        /* Bucket keys are the zero-padded bucket starts, in order. */
        EXPECT_EQ(time_series_ops_t::downsample_bucket_key(1000000000ULL),
                  agg_rows[0].first);
        EXPECT_EQ(time_series_ops_t::downsample_bucket_key(1002000000ULL),
                  agg_rows[1].first);
        /* Bucket [1000s,1002s): temps 0 and 10 → avg 5, count 2. */
        EXPECT_DOUBLE_EQ(5.0, agg_rows[0].second.get_field(
            datum_string_t("avg_temp")).as_num());
        EXPECT_DOUBLE_EQ(2.0, agg_rows[0].second.get_field(
            datum_string_t("cnt")).as_num());
        /* Bucket [1002s,1004s): temps 20 and 30 → avg 25, count 2. */
        EXPECT_DOUBLE_EQ(25.0, agg_rows[1].second.get_field(
            datum_string_t("avg_temp")).as_num());
        EXPECT_DOUBLE_EQ(2.0, agg_rows[1].second.get_field(
            datum_string_t("cnt")).as_num());

        /* Durability: a fresh catalog read sees the step root and the
         * merged watermark. */
        time_series_catalog_t reloaded = load_ts_catalog(cache_conn);
        ASSERT_EQ(1u, reloaded.downsample_roots.size());
        EXPECT_EQ(catalog.downsample_roots[0].root_block,
                  reloaded.downsample_roots[0].root_block);
        EXPECT_TRUE(reloaded.chunk_index.chunks[0].merged);
        EXPECT_EQ(2u, count_chunk_rows(
            cache_conn, reloaded.downsample_roots[0].root_block));
    });
}

}  // namespace unittest
