// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/brin.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "btree/keys.hpp"
#include "containers/archive/stl_types.hpp"
#include "containers/archive/vector_stream.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/datumspec.hpp"
#include "unittest/gtest.hpp"

namespace unittest {

namespace {

brin_summary_t make_summary(const std::string &left,
                            const std::string &right,
                            double min_val,
                            double max_val,
                            uint64_t live = 10,
                            uint64_t nulls = 0,
                            bool dirty = false) {
    brin_summary_t s;
    s.primary_key_left = store_key_t(left);
    s.primary_key_right = key_range_t::right_bound_t(store_key_t(right));
    s.minimum.push_back(ql::datum_t(min_val));
    s.maximum.push_back(ql::datum_t(max_val));
    s.live_row_count = live;
    s.null_row_count = nulls;
    s.dirty = dirty;
    return s;
}

brin_index_t make_empty_index(uint64_t range_size = 128) {
    brin_index_t idx;
    idx.format_version = BRIN_FORMAT_VERSION;
    idx.range_size = range_size;
    idx.columns.push_back("created_at");
    return idx;
}

/* BRIN-7 build-pipeline correctness helper.
 *
 * Mirrors the on-disk BRIN build loop in `build_and_persist_brin_sidecar_for_sindex`
 * (src/rdb_protocol/protocol.cc:565-605). The real code consumes (primary_key,
 * indexed_datum) pairs that the btree traversal has already SORTED by primary_key.
 * We accept the same input shape: pairs are expected in primary-key order (callers
 * may pass unsorted vectors; we sort defensively inside the helper so that callers
 * can also drive negative tests with arbitrary input). For each range of at most
 * `range_size` pairs we compute the extrema and stamp primary_key_left / right.
 *
 * Null/non-orderable datums are counted in null_row_count and skipped for min/max,
 * matching the live behaviour. */
brin_index_t simulate_brin_build(
        std::vector<std::pair<store_key_t, ql::datum_t>> entries,
        uint64_t range_size) {
    std::sort(entries.begin(), entries.end(),
              [](const std::pair<store_key_t, ql::datum_t> &a,
                 const std::pair<store_key_t, ql::datum_t> &b) {
                  return a.first < b.first;
              });

    brin_index_t idx;
    idx.format_version = BRIN_FORMAT_VERSION;
    idx.range_size = range_size;
    idx.columns.push_back("created_at");

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

        /* Seed min/max with entries[range_start].second (matches the production
         * code in protocol.cc:583-584 verbatim). The collection callback already
         * drops !has() datums, so this is safe in the real path; we still defend
         * against uninitialised datums so negative tests can drive nulls in. */
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
        /* Newly-built summaries are not dirty; only deletes/updates that grow the
         * extrema beyond a fresh rescan set dirty = true. */
        summary.dirty = false;

        idx.summaries.push_back(std::move(summary));
        range_start = range_end;
    }

    return idx;
}

}  // namespace

// ── Default construction ──

TEST(BRINSummary, DefaultConstructor) {
    brin_summary_t s;
    EXPECT_EQ(0u, s.live_row_count);
    EXPECT_EQ(0u, s.null_row_count);
    EXPECT_FALSE(s.dirty);
    EXPECT_TRUE(s.minimum.empty());
    EXPECT_TRUE(s.maximum.empty());
    EXPECT_TRUE(s.primary_key_right.unbounded);

    brin_index_t idx;
    EXPECT_EQ(BRIN_FORMAT_VERSION, idx.format_version);
    EXPECT_EQ(0u, idx.range_size);
    EXPECT_TRUE(idx.columns.empty());
    EXPECT_TRUE(idx.summaries.empty());
}

// ── Serialization round-trip: brin_summary_t ──

TEST(BRINSummary, SerializationRoundTrip) {
    brin_summary_t s1 = make_summary("aaa", "mmm", 1.0, 42.5, 100, 3, true);

    write_message_t wm;
    serialize<cluster_version_t::LATEST_DISK>(&wm, s1);

    vector_stream_t stream;
    ASSERT_EQ(0, send_write_message(&stream, &wm));

    std::vector<char> data = stream.vector();
    vector_read_stream_t read_stream(std::move(data));
    brin_summary_t s2;
    ASSERT_EQ(archive_result_t::SUCCESS,
              deserialize<cluster_version_t::LATEST_DISK>(&read_stream, &s2));

    EXPECT_EQ(s1.primary_key_left, s2.primary_key_left);
    EXPECT_EQ(s1.primary_key_right, s2.primary_key_right);
    ASSERT_EQ(1u, s2.minimum.size());
    ASSERT_EQ(1u, s2.maximum.size());
    EXPECT_EQ(0, s1.minimum[0].cmp(s2.minimum[0]));
    EXPECT_EQ(0, s1.maximum[0].cmp(s2.maximum[0]));
    EXPECT_EQ(s1.live_row_count, s2.live_row_count);
    EXPECT_EQ(s1.null_row_count, s2.null_row_count);
    EXPECT_EQ(s1.dirty, s2.dirty);
}

// ── Serialization round-trip: brin_index_t ──

TEST(BRINIndex, SerializationRoundTrip) {
    brin_index_t idx1 = make_empty_index(256);
    idx1.summaries.push_back(make_summary("a", "m", 10.0, 20.0, 50));
    idx1.summaries.push_back(make_summary("m", "z", 5.0, 30.0, 60, 1, true));

    write_message_t wm;
    serialize<cluster_version_t::LATEST_DISK>(&wm, idx1);

    vector_stream_t stream;
    ASSERT_EQ(0, send_write_message(&stream, &wm));

    std::vector<char> data = stream.vector();
    vector_read_stream_t read_stream(std::move(data));
    brin_index_t idx2;
    ASSERT_EQ(archive_result_t::SUCCESS,
              deserialize<cluster_version_t::LATEST_DISK>(&read_stream, &idx2));

    EXPECT_EQ(idx1.format_version, idx2.format_version);
    EXPECT_EQ(idx1.range_size, idx2.range_size);
    ASSERT_EQ(idx1.columns.size(), idx2.columns.size());
    EXPECT_EQ(idx1.columns[0], idx2.columns[0]);
    ASSERT_EQ(2u, idx2.summaries.size());
    EXPECT_EQ(idx1.summaries[0].primary_key_left,
              idx2.summaries[0].primary_key_left);
    EXPECT_EQ(idx1.summaries[1].live_row_count,
              idx2.summaries[1].live_row_count);
    EXPECT_TRUE(idx2.summaries[1].dirty);
    EXPECT_EQ(0, idx1.summaries[0].minimum[0].cmp(idx2.summaries[0].minimum[0]));
}

// ── Validation: valid cases ──

TEST(BRINIndex, ValidateEmptyIndex) {
    brin_index_t idx = make_empty_index();
    std::string err;
    EXPECT_TRUE(validate_brin_index(idx, &err)) << err;
}

TEST(BRINIndex, ValidateSingleSummary) {
    brin_index_t idx = make_empty_index();
    idx.summaries.push_back(make_summary("a", "z", 1.0, 100.0));
    std::string err;
    EXPECT_TRUE(validate_brin_index(idx, &err)) << err;
}

TEST(BRINIndex, ValidateMultiSummarySorted) {
    brin_index_t idx = make_empty_index();
    idx.summaries.push_back(make_summary("a", "m", 1.0, 10.0));
    idx.summaries.push_back(make_summary("m", "t", 0.0, 50.0));
    idx.summaries.push_back(make_summary("t", "z", 5.0, 5.0));
    std::string err;
    EXPECT_TRUE(validate_brin_index(idx, &err)) << err;
}

// ── Validation: rejection cases ──

TEST(BRINIndex, ValidateRejectsBadFormatVersion) {
    brin_index_t idx = make_empty_index();
    idx.format_version = 99;
    std::string err;
    EXPECT_FALSE(validate_brin_index(idx, &err));
    EXPECT_FALSE(err.empty());
}

TEST(BRINIndex, ValidateRejectsZeroRangeSize) {
    brin_index_t idx = make_empty_index();
    idx.range_size = 0;
    std::string err;
    EXPECT_FALSE(validate_brin_index(idx, &err));
    EXPECT_FALSE(err.empty());
}

TEST(BRINIndex, ValidateRejectsOutOfRangeSize) {
    brin_index_t idx = make_empty_index();
    idx.range_size = 8;  // below BRIN_RANGE_SIZE_MIN
    std::string err;
    EXPECT_FALSE(validate_brin_index(idx, &err));

    idx.range_size = 100000;  // above BRIN_RANGE_SIZE_MAX
    EXPECT_FALSE(validate_brin_index(idx, &err));
}

TEST(BRINIndex, ValidateRejectsEmptyColumns) {
    brin_index_t idx;
    idx.format_version = BRIN_FORMAT_VERSION;
    idx.range_size = 128;
    // columns intentionally empty
    std::string err;
    EXPECT_FALSE(validate_brin_index(idx, &err));
}

TEST(BRINIndex, ValidateRejectsColumnCountMismatch) {
    brin_index_t idx = make_empty_index();
    brin_summary_t s = make_summary("a", "z", 1.0, 2.0);
    // Add a second min without matching columns
    s.minimum.push_back(ql::datum_t(3.0));
    s.maximum.push_back(ql::datum_t(4.0));
    idx.summaries.push_back(s);
    std::string err;
    EXPECT_FALSE(validate_brin_index(idx, &err));
}

TEST(BRINIndex, ValidateRejectsUnsortedSummaries) {
    brin_index_t idx = make_empty_index();
    // Second range starts before first ends and is out of order
    idx.summaries.push_back(make_summary("m", "z", 1.0, 2.0));
    idx.summaries.push_back(make_summary("a", "c", 3.0, 4.0));
    std::string err;
    EXPECT_FALSE(validate_brin_index(idx, &err));
}

TEST(BRINIndex, ValidateRejectsOverlappingSummaries) {
    brin_index_t idx = make_empty_index();
    idx.summaries.push_back(make_summary("a", "m", 1.0, 2.0));
    // Overlaps previous: left "c" is still before previous right "m"
    idx.summaries.push_back(make_summary("c", "z", 3.0, 4.0));
    std::string err;
    EXPECT_FALSE(validate_brin_index(idx, &err));
}

TEST(BRINIndex, ValidateRejectsMinGreaterThanMax) {
    brin_index_t idx = make_empty_index();
    idx.summaries.push_back(make_summary("a", "z", 100.0, 1.0));  // min > max
    std::string err;
    EXPECT_FALSE(validate_brin_index(idx, &err));
}

TEST(BRINSummary, ValidateRejectsMinGreaterThanMax) {
    std::vector<std::string> cols;
    cols.push_back("x");
    brin_summary_t s = make_summary("a", "z", 50.0, 10.0);
    std::string err;
    EXPECT_FALSE(validate_brin_summary(s, cols, &err));
}

// ──────────────────────────────────────────────────────────────────────
// BRIN-7 build-pipeline correctness tests.
//
// These drive `simulate_brin_build()` (which mirrors the on-disk build loop
// in src/rdb_protocol/protocol.cc:565-605) and assert invariants about the
// produced `brin_index_t` for known inputs. They do NOT spin up a real
// store; the on-disk round-trip would require an entire sindex B-tree and
// would duplicate what `build_and_persist_brin_sidecar_for_sindex` already
// exercises in production. Instead, they pin down the corner cases the
// production loop must respect: boundary equality, last-range unbounded,
// null rejection, single/small inputs, etc.
// ──────────────────────────────────────────────────────────────────────

TEST(BRINBuild, CorrectRangeBoundaries) {
    /* Five entries, range_size=2 → ranges covering [0,2),[2,4),[4,5).
     * Right bound of range i must equal primary_key_left of range i+1, and the
     * final range's right bound must be unbounded. */
    std::vector<std::pair<store_key_t, ql::datum_t>> entries;
    for (int i = 0; i < 5; ++i) {
        char key = static_cast<char>('a' + i);
        std::string s(1, key);
        entries.emplace_back(store_key_t(s), ql::datum_t(static_cast<double>(i)));
    }
    brin_index_t idx = simulate_brin_build(entries, 2);
    ASSERT_EQ(3u, idx.summaries.size());

    EXPECT_EQ(store_key_t("a"), idx.summaries[0].primary_key_left);
    EXPECT_EQ(key_range_t::right_bound_t(store_key_t("c")),
              idx.summaries[0].primary_key_right);
    EXPECT_EQ(store_key_t("c"), idx.summaries[1].primary_key_left);
    EXPECT_EQ(key_range_t::right_bound_t(store_key_t("e")),
              idx.summaries[1].primary_key_right);
    EXPECT_EQ(store_key_t("e"), idx.summaries[2].primary_key_left);
    EXPECT_TRUE(idx.summaries[2].primary_key_right.unbounded);
}

TEST(BRINBuild, MinMaxCorrectness) {
    /* Pairs chosen so the min and max per range are NOT the first entry —
     * this guards against accidentally returning entries[range_start] as both
     * extrema. */
    std::vector<std::pair<store_key_t, ql::datum_t>> entries = {
        {store_key_t("a"), ql::datum_t(15.0)},
        {store_key_t("b"), ql::datum_t(3.0)},
        {store_key_t("c"), ql::datum_t(7.0)},
        {store_key_t("d"), ql::datum_t(2.0)},
        {store_key_t("e"), ql::datum_t(11.0)},
        {store_key_t("f"), ql::datum_t(99.0)},
        {store_key_t("g"), ql::datum_t(5.0)},
        {store_key_t("h"), ql::datum_t(4.0)},
    };
    brin_index_t idx = simulate_brin_build(entries, 4);
    ASSERT_EQ(2u, idx.summaries.size());

    /* Range 1: a..d → values 15,3,7,2 → min 2, max 15 */
    EXPECT_EQ(0, idx.summaries[0].minimum[0].cmp(ql::datum_t(2.0)));
    EXPECT_EQ(0, idx.summaries[0].maximum[0].cmp(ql::datum_t(15.0)));
    /* Range 2: e..h → values 11,99,5,4 → min 4, max 99 */
    EXPECT_EQ(0, idx.summaries[1].minimum[0].cmp(ql::datum_t(4.0)));
    EXPECT_EQ(0, idx.summaries[1].maximum[0].cmp(ql::datum_t(99.0)));
}

TEST(BRINBuild, ExactRangeSizeGrouping) {
    /* Exactly 3*range_size entries → 3 ranges of exactly range_size each;
     * confirm the per-range live_row_count totals. */
    const uint64_t range_size = 4;
    const size_t total = 3 * range_size;
    std::vector<std::pair<store_key_t, ql::datum_t>> entries;
    for (size_t i = 0; i < total; ++i) {
        std::string key(1, static_cast<char>('a' + i));
        entries.emplace_back(store_key_t(key), ql::datum_t(static_cast<double>(i)));
    }
    brin_index_t idx = simulate_brin_build(entries, range_size);
    ASSERT_EQ(3u, idx.summaries.size());
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(range_size, idx.summaries[i].live_row_count);
        EXPECT_EQ(0u, idx.summaries[i].null_row_count);
    }
}

TEST(BRINBuild, SingleEntry) {
    std::vector<std::pair<store_key_t, ql::datum_t>> entries = {
        {store_key_t("xyz"), ql::datum_t(42.0)},
    };
    brin_index_t idx = simulate_brin_build(entries, 128);
    ASSERT_EQ(1u, idx.summaries.size());
    EXPECT_EQ(store_key_t("xyz"), idx.summaries[0].primary_key_left);
    EXPECT_TRUE(idx.summaries[0].primary_key_right.unbounded);
    EXPECT_EQ(1u, idx.summaries[0].live_row_count);
    EXPECT_EQ(0u, idx.summaries[0].null_row_count);
    EXPECT_EQ(0, idx.summaries[0].minimum[0].cmp(ql::datum_t(42.0)));
    EXPECT_EQ(0, idx.summaries[0].maximum[0].cmp(ql::datum_t(42.0)));
}

TEST(BRINBuild, MultipleRangesExactSize) {
    /* 2 * range_size entries → exactly 2 ranges, each exactly range_size wide. */
    const uint64_t range_size = 16;
    std::vector<std::pair<store_key_t, ql::datum_t>> entries;
    for (size_t i = 0; i < 2 * range_size; ++i) {
        std::string key(1, static_cast<char>('a' + (i % 26)));
        std::string full;
        for (size_t j = 0; j <= i / 26; ++j) full.push_back('a' + (i % 26));
        entries.emplace_back(store_key_t(full), ql::datum_t(static_cast<double>(i)));
    }
    brin_index_t idx = simulate_brin_build(entries, range_size);
    ASSERT_EQ(2u, idx.summaries.size());
    EXPECT_EQ(range_size, idx.summaries[0].live_row_count);
    EXPECT_EQ(range_size, idx.summaries[1].live_row_count);
    /* Last range is unbounded. */
    EXPECT_TRUE(idx.summaries[1].primary_key_right.unbounded);
}

TEST(BRINBuild, LastRangeUnbounded) {
    /* For ANY number of summaries, summaries.back().right must be unbounded. */
    std::vector<std::pair<store_key_t, ql::datum_t>> entries;
    for (int i = 0; i < 7; ++i) {
        std::string key(1, static_cast<char>('a' + i));
        entries.emplace_back(store_key_t(key), ql::datum_t(static_cast<double>(i)));
    }
    brin_index_t idx = simulate_brin_build(entries, 3);
    ASSERT_EQ(3u, idx.summaries.size());
    for (size_t i = 0; i + 1 < idx.summaries.size(); ++i) {
        EXPECT_FALSE(idx.summaries[i].primary_key_right.unbounded);
    }
    EXPECT_TRUE(idx.summaries.back().primary_key_right.unbounded);
}

TEST(BRINBuild, NullRejection) {
    /* Insert one uninitialised datum between two valid ones. The summary's
     * live_row_count should be 2, null_row_count should be 1, and min/max
     * should bracket the two valid values (excluding the null). */
    std::vector<std::pair<store_key_t, ql::datum_t>> entries = {
        {store_key_t("a"), ql::datum_t(10.0)},
        {store_key_t("b"), ql::datum_t()},
        {store_key_t("c"), ql::datum_t(20.0)},
    };
    brin_index_t idx = simulate_brin_build(entries, 10);
    ASSERT_EQ(1u, idx.summaries.size());
    EXPECT_EQ(2u, idx.summaries[0].live_row_count);
    EXPECT_EQ(1u, idx.summaries[0].null_row_count);
    EXPECT_EQ(0, idx.summaries[0].minimum[0].cmp(ql::datum_t(10.0)));
    EXPECT_EQ(0, idx.summaries[0].maximum[0].cmp(ql::datum_t(20.0)));
}

TEST(BRINBuild, LargeRangeSize) {
    /* range_size far above total entries must produce exactly one summary. */
    std::vector<std::pair<store_key_t, ql::datum_t>> entries;
    for (int i = 0; i < 3; ++i) {
        std::string key(1, static_cast<char>('a' + i));
        entries.emplace_back(store_key_t(key), ql::datum_t(static_cast<double>(i)));
    }
    brin_index_t idx = simulate_brin_build(entries, 1024);
    ASSERT_EQ(1u, idx.summaries.size());
    EXPECT_TRUE(idx.summaries[0].primary_key_right.unbounded);
    EXPECT_EQ(3u, idx.summaries[0].live_row_count);
    EXPECT_EQ(0, idx.summaries[0].minimum[0].cmp(ql::datum_t(0.0)));
    EXPECT_EQ(0, idx.summaries[0].maximum[0].cmp(ql::datum_t(2.0)));
}

TEST(BRINBuild, DirtyFlag) {
    /* Every freshly-built summary must report dirty=false. */
    std::vector<std::pair<store_key_t, ql::datum_t>> entries;
    for (int i = 0; i < 4; ++i) {
        std::string key(1, static_cast<char>('a' + i));
        entries.emplace_back(store_key_t(key), ql::datum_t(static_cast<double>(i)));
    }
    brin_index_t idx = simulate_brin_build(entries, 2);
    ASSERT_EQ(2u, idx.summaries.size());
    EXPECT_FALSE(idx.summaries[0].dirty);
    EXPECT_FALSE(idx.summaries[1].dirty);
}

TEST(BRINBuild, ResultValidates) {
    /* Whatever simulate_brin_build() emits should pass validate_brin_index().
     * This is the strongest invariant: the production code should never
     * produce an invalid sidecar. range_size must lie in [16, 65536]. */
    std::vector<std::pair<store_key_t, ql::datum_t>> entries;
    for (int i = 0; i < 50; ++i) {
        std::string key(1, static_cast<char>('a' + (i % 26)));
        entries.emplace_back(store_key_t(key), ql::datum_t(static_cast<double>(i)));
    }
    brin_index_t idx = simulate_brin_build(entries, 17);
    std::string err;
    EXPECT_TRUE(validate_brin_index(idx, &err)) << err;
}

TEST(BRINBuild, UnsortedInputGetsSorted) {
    /* Pass entries in non-sorted order — simulate_brin_build() must sort them
     * before range grouping, so the output is still a valid sidecar.
     * range_size must lie in [16, 65536]. */
    std::vector<std::pair<store_key_t, ql::datum_t>> entries = {
        {store_key_t("d"), ql::datum_t(4.0)},
        {store_key_t("a"), ql::datum_t(1.0)},
        {store_key_t("c"), ql::datum_t(3.0)},
        {store_key_t("b"), ql::datum_t(2.0)},
    };
    brin_index_t idx = simulate_brin_build(entries, 16);
    ASSERT_EQ(1u, idx.summaries.size());
    /* After sort: range covers {a, b, c, d} (fits in one range of 16 entries). */
    EXPECT_EQ(store_key_t("a"), idx.summaries[0].primary_key_left);
    EXPECT_TRUE(idx.summaries[0].primary_key_right.unbounded);
    std::string err;
    EXPECT_TRUE(validate_brin_index(idx, &err)) << err;
}

// ──────────────────────────────────────────────────────────────────────
// BRIN-7 pruning-logic correctness tests.
//
// These exercise the same comparison
// `query_range.overlaps_closed_interval(summary.minimum[0], summary.maximum[0])`
// that store.cc:782-802 uses for summary pruning.
// ──────────────────────────────────────────────────────────────────────

namespace {

/* Mimic the store.cc pruning loop: a summary is kept iff it has live rows,
 * non-empty min/max, and `query_range.overlaps_closed_interval(min, max)`.
 * The pruning function is intentionally a tiny shim over the production
 * helper so tests exercise the same primitives. */
bool prune_summary(
        const brin_summary_t &summary,
        const ql::datum_range_t &query_range) {
    if (summary.live_row_count == 0
        || summary.minimum.empty()
        || summary.maximum.empty()) {
        return false;
    }
    return query_range.overlaps_closed_interval(
        summary.minimum[0], summary.maximum[0]);
}

/* A summary survivor list is what would become `candidate_ranges` in
 * store.cc. Tests feed arrays of summaries + a query and assert which
 * survive pruning. */
std::vector<size_t> survivors(
        const std::vector<brin_summary_t> &summaries,
        const ql::datum_range_t &query_range) {
    std::vector<size_t> out;
    for (size_t i = 0; i < summaries.size(); ++i) {
        if (prune_summary(summaries[i], query_range)) out.push_back(i);
    }
    return out;
}

ql::datum_range_t closed_range(double lo, double hi) {
    return ql::datum_range_t(
        ql::datum_t(lo), key_range_t::closed,
        ql::datum_t(hi), key_range_t::closed);
}

ql::datum_range_t open_lo_closed_hi(double lo, double hi) {
    return ql::datum_range_t(
        ql::datum_t(lo), key_range_t::open,
        ql::datum_t(hi), key_range_t::closed);
}

}  // namespace

TEST(BRINPruning, SummaryOverlapsQueryRange) {
    /* Summary [5,15] overlaps query [10,20] at 10..15 → kept. */
    brin_summary_t s = make_summary("a", "m", 5.0, 15.0);
    EXPECT_FALSE(s.minimum.empty());
    ql::datum_range_t q = closed_range(10.0, 20.0);
    EXPECT_TRUE(prune_summary(s, q));
}

TEST(BRINPruning, SummaryBelowQueryRange) {
    /* Summary [1,4] entirely left of query [5,10] → pruned. */
    brin_summary_t s = make_summary("a", "z", 1.0, 4.0);
    ql::datum_range_t q = closed_range(5.0, 10.0);
    EXPECT_FALSE(prune_summary(s, q));
}

TEST(BRINPruning, SummaryAboveQueryRange) {
    /* Summary [20,30] entirely right of query [5,10] → pruned. */
    brin_summary_t s = make_summary("a", "z", 20.0, 30.0);
    ql::datum_range_t q = closed_range(5.0, 10.0);
    EXPECT_FALSE(prune_summary(s, q));
}

TEST(BRINPruning, SummaryContainsQueryRange) {
    /* Summary [1,100] contains query [40,60] → kept. */
    brin_summary_t s = make_summary("a", "z", 1.0, 100.0);
    ql::datum_range_t q = closed_range(40.0, 60.0);
    EXPECT_TRUE(prune_summary(s, q));
}

TEST(BRINPruning, SinglePointQueryAtSummaryMax) {
    /* Summary [5,15] against single-point query at 15 → kept (closed overlap). */
    brin_summary_t s = make_summary("a", "z", 5.0, 15.0);
    ql::datum_range_t q(ql::datum_t(15.0));
    EXPECT_TRUE(prune_summary(s, q));
}

TEST(BRINPruning, SinglePointQueryBelowSummary) {
    /* Single-point query at 1 (entirely below summary [5,15]) → pruned. */
    brin_summary_t s = make_summary("a", "z", 5.0, 15.0);
    ql::datum_range_t q(ql::datum_t(1.0));
    EXPECT_FALSE(prune_summary(s, q));
}

TEST(BRINPruning, QueryEqualsSummaryBounds) {
    /* Query and summary share exact bounds → still kept (closed overlap). */
    brin_summary_t s = make_summary("a", "z", 10.0, 20.0);
    ql::datum_range_t q = closed_range(10.0, 20.0);
    EXPECT_TRUE(prune_summary(s, q));
}

TEST(BRINPruning, OpenLowerBoundSemantics) {
    /* `overlaps_closed_interval` is conservative: any non-empty intersection
     * between the summary [min, max] and the query range keeps the summary.
     * The open/closed flag on the *query* is honoured. */
    ql::datum_range_t open_q = open_lo_closed_hi(5.0, 10.0);  // (5, 10]

    /* Summary entirely below 5 → pruned. */
    brin_summary_t below = make_summary("a", "z", 1.0, 4.0);
    EXPECT_FALSE(prune_summary(below, open_q));

    /* Summary entirely above 10 → pruned. */
    brin_summary_t above = make_summary("a", "z", 11.0, 20.0);
    EXPECT_FALSE(prune_summary(above, open_q));

    /* Summary [5, 15] vs query (5, 10]: the conservative overlap returns true
     * (5..15 and 5..10 share at least (5, 10]). This is intentionally
     * over-permissive for the prune path — a false positive would just mean
     * a needless B-tree scan, never a wrong result. */
    brin_summary_t touching = make_summary("a", "z", 5.0, 15.0);
    EXPECT_TRUE(prune_summary(touching, open_q));

    /* Summary [1, 6] vs query (5, 10]: same — overlaps at (5, 6]. */
    brin_summary_t crossing = make_summary("a", "z", 1.0, 6.0);
    EXPECT_TRUE(prune_summary(crossing, open_q));
}

TEST(BRINPruning, OpenRightBoundSemantics) {
    /* Mirror of the previous test for an open right bound. Query [5, 10) —
     * closed at 5, open at 10. `overlaps_closed_interval` honours the open
     * right flag: when summary.min == 10 == query.right and query.right is
     * open, the comparison is treated as exclusive and the summary gets
     * pruned (the summary covers [10, 12] which doesn't intersect (5, 10)). */
    ql::datum_range_t q = ql::datum_range_t(
        ql::datum_t(5.0), key_range_t::closed,
        ql::datum_t(10.0), key_range_t::open);

    /* Summary [10, 12] touches 10 but is open-excluded → pruned. */
    brin_summary_t touching = make_summary("a", "z", 10.0, 12.0);
    EXPECT_FALSE(prune_summary(touching, q));

    /* Summary [10.5, 12] strictly above → pruned. */
    brin_summary_t above = make_summary("a", "z", 10.5, 12.0);
    EXPECT_FALSE(prune_summary(above, q));

    /* Summary [5, 9] fully inside → kept. */
    brin_summary_t inside = make_summary("a", "z", 5.0, 9.0);
    EXPECT_TRUE(prune_summary(inside, q));
}

TEST(BRINPruning, EmptyLiveRowsSkipped) {
    /* Pruning logic explicitly skips summaries with live_row_count == 0
     * (store.cc:783). */
    brin_summary_t empty = make_summary("a", "z", 1.0, 100.0, 0);
    ql::datum_range_t q = closed_range(50.0, 60.0);
    EXPECT_FALSE(prune_summary(empty, q));
}

TEST(BRINPruning, MixedSummaries) {
    /* Three summaries: one below, one overlapping, one above. Only the
     * overlapping one survives. */
    std::vector<brin_summary_t> sums = {
        make_summary("a", "k", 1.0, 4.0),     // below
        make_summary("k", "p", 5.0, 25.0),    // overlaps [10,20]
        make_summary("p", "z", 30.0, 40.0),   // above
    };
    ql::datum_range_t q = closed_range(10.0, 20.0);
    std::vector<size_t> kept = survivors(sums, q);
    ASSERT_EQ(1u, kept.size());
    EXPECT_EQ(1u, kept[0]);
}

// ──────────────────────────────────────────────────────────────────────
// BRIN-7 additional validation edge cases.
// ──────────────────────────────────────────────────────────────────────

TEST(BRINValidation, NullMinimumDatum) {
    /* A summary whose min datum is uninitialised must fail. The validator
     * checks `.has()` for both min and max (brin.cc:33). */
    brin_index_t idx = make_empty_index();
    brin_summary_t s;
    s.primary_key_left = store_key_t("a");
    s.primary_key_right = key_range_t::right_bound_t(store_key_t("z"));
    s.minimum.push_back(ql::datum_t());  // uninitialised
    s.maximum.push_back(ql::datum_t(10.0));
    s.live_row_count = 1;
    idx.summaries.push_back(s);

    std::string err;
    EXPECT_FALSE(validate_brin_index(idx, &err));
    EXPECT_NE(std::string::npos, err.find("uninitialized"))
        << "got err=" << err;
}

TEST(BRINValidation, NullMaximumDatum) {
    brin_index_t idx = make_empty_index();
    brin_summary_t s;
    s.primary_key_left = store_key_t("a");
    s.primary_key_right = key_range_t::right_bound_t(store_key_t("z"));
    s.minimum.push_back(ql::datum_t(0.0));
    s.maximum.push_back(ql::datum_t());  // uninitialised
    s.live_row_count = 1;
    idx.summaries.push_back(s);

    std::string err;
    EXPECT_FALSE(validate_brin_index(idx, &err));
    EXPECT_NE(std::string::npos, err.find("uninitialized")) << err;
}

TEST(BRINValidation, UnboundedRightNotLast) {
    /* A summary with unbounded right followed by another summary must fail
     * (the unbounded summary must be last; brin.cc:101-107). */
    brin_index_t idx = make_empty_index();
    brin_summary_t first;
    first.primary_key_left = store_key_t("a");
    first.primary_key_right =
        key_range_t::right_bound_t::make_unbounded();
    first.minimum.push_back(ql::datum_t(1.0));
    first.maximum.push_back(ql::datum_t(2.0));
    first.live_row_count = 1;
    idx.summaries.push_back(first);
    idx.summaries.push_back(make_summary("z", "z9", 3.0, 4.0));

    std::string err;
    EXPECT_FALSE(validate_brin_index(idx, &err));
    EXPECT_NE(std::string::npos, err.find("unbounded")) << err;
}

TEST(BRINValidation, ZeroLiveRowsValid) {
    /* An empty range (live_row_count=0) is legal — the summary still records
     * its PK interval but no rows live there. */
    brin_index_t idx = make_empty_index();
    idx.summaries.push_back(make_summary("a", "z", 1.0, 2.0, 0));
    std::string err;
    EXPECT_TRUE(validate_brin_index(idx, &err)) << err;
}

TEST(BRINValidation, EmptyColumnName) {
    /* A column entry that is the empty string must fail; column names must be
     * non-empty (brin.cc:75-82). */
    brin_index_t idx;
    idx.format_version = BRIN_FORMAT_VERSION;
    idx.range_size = 128;
    idx.columns.push_back("ok_col");
    idx.columns.push_back("");  // empty → fail
    idx.summaries.push_back(make_summary("a", "z", 1.0, 2.0, 1));
    /* Add a second min/max to satisfy the size check. */
    idx.summaries[0].minimum.push_back(ql::datum_t(0.0));
    idx.summaries[0].maximum.push_back(ql::datum_t(5.0));

    std::string err;
    EXPECT_FALSE(validate_brin_index(idx, &err));
    EXPECT_NE(std::string::npos, err.find("non-empty")) << err;
}

TEST(BRINValidation, MultipleColumnsValid) {
    /* A two-column summary whose min/max each have 2 elements validates. */
    brin_index_t idx;
    idx.format_version = BRIN_FORMAT_VERSION;
    idx.range_size = 128;
    idx.columns.push_back("a");
    idx.columns.push_back("b");

    brin_summary_t s;
    s.primary_key_left = store_key_t("k0");
    s.primary_key_right = key_range_t::right_bound_t(store_key_t("k9"));
    s.minimum.push_back(ql::datum_t(1.0));
    s.minimum.push_back(ql::datum_t(2.0));
    s.maximum.push_back(ql::datum_t(10.0));
    s.maximum.push_back(ql::datum_t(20.0));
    s.live_row_count = 5;
    idx.summaries.push_back(s);

    std::string err;
    EXPECT_TRUE(validate_brin_index(idx, &err)) << err;
}

TEST(BRINValidation, MultipleColumnsMismatchedMinMax) {
    /* Two columns but min has 1 element while max has 2 → fail
     * (the size check in brin.cc:14 runs before the per-element checks). */
    brin_index_t idx;
    idx.format_version = BRIN_FORMAT_VERSION;
    idx.range_size = 128;
    idx.columns.push_back("a");
    idx.columns.push_back("b");

    brin_summary_t s;
    s.primary_key_left = store_key_t("k0");
    s.primary_key_right = key_range_t::right_bound_t(store_key_t("k9"));
    s.minimum.push_back(ql::datum_t(1.0));          // only 1
    s.maximum.push_back(ql::datum_t(10.0));
    s.maximum.push_back(ql::datum_t(20.0));         // 2
    s.live_row_count = 5;
    idx.summaries.push_back(s);

    std::string err;
    EXPECT_FALSE(validate_brin_index(idx, &err));
    EXPECT_NE(std::string::npos,
              err.find("min/max size does not match column count")) << err;
}

TEST(BRINValidation, MultipleColumnsMinGreaterInSecondColumn) {
    /* When min > max in column 1, the validator rejects (brin.cc:39). */
    brin_index_t idx;
    idx.format_version = BRIN_FORMAT_VERSION;
    idx.range_size = 128;
    idx.columns.push_back("a");
    idx.columns.push_back("b");

    brin_summary_t s;
    s.primary_key_left = store_key_t("k0");
    s.primary_key_right = key_range_t::right_bound_t(store_key_t("k9"));
    s.minimum.push_back(ql::datum_t(1.0));
    s.minimum.push_back(ql::datum_t(99.0));
    s.maximum.push_back(ql::datum_t(10.0));
    s.maximum.push_back(ql::datum_t(5.0));  // 99 > 5 in column 1
    s.live_row_count = 5;
    idx.summaries.push_back(s);

    std::string err;
    EXPECT_FALSE(validate_brin_index(idx, &err));
    EXPECT_NE(std::string::npos, err.find("greater than maximum")) << err;
}

}  // namespace unittest
