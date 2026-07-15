// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/brin.hpp"

#include <string>
#include <vector>

#include "containers/archive/stl_types.hpp"
#include "containers/archive/vector_stream.hpp"
#include "rdb_protocol/datum.hpp"
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

}  // namespace unittest
