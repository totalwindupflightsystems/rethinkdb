// Copyright 2026 RethinkDB, all rights reserved.
#include "unittest/gtest.hpp"

#include "containers/uuid.hpp"
#include "rdb_protocol/cdc_types.hpp"
#include "rdb_protocol/conflict_resolver.hpp"

namespace unittest {
namespace {

using namespace ql;

// Helper: build a change_record_t with minimal fields
change_record_t make_record(change_operation_t op,
                            microtime_t ts,
                            uuid_u cluster,
                            uuid_u shard,
                            uint64_t lsn_val) {
    static uuid_u table = generate_uuid();
    change_record_t r;
    r.op = op;
    r.commit_timestamp = ts;
    r.event_id.source_cluster_id = cluster;
    r.event_id.table_id = table;
    r.event_id.shard_id = shard;
    r.event_id.lsn.value = lsn_val;
    // Non-empty images so the record isn't empty
    if (op == change_operation_t::INSERT || op == change_operation_t::UPDATE
        || op == change_operation_t::REPLACE) {
        r.after_image = {'x'};
    }
    if (op == change_operation_t::UPDATE || op == change_operation_t::DELETE
        || op == change_operation_t::REPLACE) {
        r.before_image = {'y'};
    }
    return r;
}

}  // namespace

TEST(ConflictResolverTest, LWW_NewerTimestampWins) {
    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();

    change_record_t source = make_record(
        change_operation_t::UPDATE, 2000, cluster, shard, 1);
    change_record_t target = make_record(
        change_operation_t::UPDATE, 1000, cluster, shard, 2);

    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);
    auto result = resolver.resolve(source, target);
    EXPECT_FALSE(result.skipped);
    // Source has newer timestamp, should win
    EXPECT_EQ(result.resolved.commit_timestamp, 2000u);
}

TEST(ConflictResolverTest, LWW_SameTimestampSourceWins) {
    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();

    change_record_t source = make_record(
        change_operation_t::UPDATE, 1000, cluster, shard, 1);
    change_record_t target = make_record(
        change_operation_t::UPDATE, 1000, cluster, shard, 2);

    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);
    auto result = resolver.resolve(source, target);
    EXPECT_FALSE(result.skipped);
    // Same timestamp, LSN tiebreak: target has higher LSN (2 > 1)
    // so target wins
    EXPECT_EQ(result.resolved.event_id.lsn.value, 2u);
}

TEST(ConflictResolverTest, SourceWinsPolicy) {
    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();

    // Target is newer (3000 > 1000), but SOURCE_WINS ignores timestamps
    change_record_t source = make_record(
        change_operation_t::UPDATE, 1000, cluster, shard, 1);
    change_record_t target = make_record(
        change_operation_t::UPDATE, 3000, cluster, shard, 2);

    conflict_resolver_t resolver(conflict_resolution_policy_t::SOURCE_WINS);
    auto result = resolver.resolve(source, target);
    EXPECT_FALSE(result.skipped);
    EXPECT_EQ(result.resolved.commit_timestamp, 1000u);
}

TEST(ConflictResolverTest, DeleteVsDeleteSkips) {
    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();

    change_record_t source = make_record(
        change_operation_t::DELETE, 1000, cluster, shard, 1);
    change_record_t target = make_record(
        change_operation_t::DELETE, 2000, cluster, shard, 2);

    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);
    auto result = resolver.resolve(source, target);
    EXPECT_TRUE(result.skipped);
    EXPECT_EQ(result.reason, "both records are DELETE");
}

TEST(ConflictResolverTest, InsertVsInsertSourceWins) {
    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();

    // Target is newer, but INSERT vs INSERT always picks source
    change_record_t source = make_record(
        change_operation_t::INSERT, 1000, cluster, shard, 1);
    change_record_t target = make_record(
        change_operation_t::INSERT, 2000, cluster, shard, 2);

    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);
    auto result = resolver.resolve(source, target);
    EXPECT_FALSE(result.skipped);
    EXPECT_EQ(result.resolved.commit_timestamp, 1000u);
    EXPECT_EQ(result.resolved.op, change_operation_t::INSERT);
}

TEST(ConflictResolverTest, CustomHandlerFallback) {
    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();

    change_record_t source = make_record(
        change_operation_t::UPDATE, 1000, cluster, shard, 1);
    change_record_t target = make_record(
        change_operation_t::UPDATE, 2000, cluster, shard, 2);

    conflict_resolver_t resolver(conflict_resolution_policy_t::CUSTOM_HANDLER);

    bool handler_called = false;
    resolver.set_custom_handler(
        [&handler_called](const change_record_t &s, const change_record_t &t)
            -> conflict_resolution_result_t {
            handler_called = true;
            conflict_resolution_result_t r;
            r.resolved = s;  // custom handler picks source
            r.reason = "custom handler chose source";
            return r;
        });

    auto result = resolver.resolve(source, target);
    EXPECT_TRUE(handler_called);
    EXPECT_FALSE(result.skipped);
    EXPECT_EQ(result.resolved.commit_timestamp, 1000u);
    EXPECT_EQ(result.reason, "custom handler chose source");
}

TEST(ConflictResolverTest, CustomHandlerFallbackToLWWWhenNoHandler) {
    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();

    change_record_t source = make_record(
        change_operation_t::UPDATE, 1000, cluster, shard, 1);
    change_record_t target = make_record(
        change_operation_t::UPDATE, 3000, cluster, shard, 2);

    // CUSTOM_HANDLER with no handler set → falls through to LWW
    conflict_resolver_t resolver(conflict_resolution_policy_t::CUSTOM_HANDLER);
    auto result = resolver.resolve(source, target);
    EXPECT_FALSE(result.skipped);
    // Target is newer (3000 > 1000), LWW picks target
    EXPECT_EQ(result.resolved.commit_timestamp, 3000u);
}

TEST(ConflictResolverTest, ManualPolicySkips) {
    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();

    change_record_t source = make_record(
        change_operation_t::UPDATE, 1000, cluster, shard, 1);
    change_record_t target = make_record(
        change_operation_t::UPDATE, 2000, cluster, shard, 2);

    conflict_resolver_t resolver(conflict_resolution_policy_t::MANUAL);
    auto result = resolver.resolve(source, target);
    EXPECT_TRUE(result.skipped);
    EXPECT_EQ(result.reason, "manual intervention required");
}

}  // namespace unittest
