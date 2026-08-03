// Copyright 2026 RethinkDB, all rights reserved.
/* PHASE3-TS-1 / spec §8.1:
 *   - chunk index: insert, overlap query, boundary conditions
 *   - serialization: round-trip time_series_config_t, time_chunk_index_t
 *   - downsample selection: correct resolution for range sizes
 *   - retention validation: exactly-at-TTL, just-before, just-after
 * Plus table_config_t integration (field serialization + equality). */
#include "btree/time_series_config.hpp"
#include "btree/time_chunk.hpp"

#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "clustering/administration/tables/table_metadata.hpp"
#include "containers/archive/vector_stream.hpp"
#include "containers/archive/versioned.hpp"
#include "containers/name_string.hpp"
#include "containers/uuid.hpp"
#include "rdb_protocol/error.hpp"
#include "unittest/gtest.hpp"

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

}  // namespace unittest
