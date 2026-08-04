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
#include "rdb_protocol/terms/time_series.hpp"

#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "arch/io/disk.hpp"
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
#include "rdb_protocol/protocol.hpp"
#include "rdb_protocol/pseudo_time.hpp"
#include "rdb_protocol/time_series_errors.hpp"
#include "serializer/log/log_serializer.hpp"
#include "unittest/gtest.hpp"
#include "unittest/unittest_utils.hpp"

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

}  // namespace unittest
