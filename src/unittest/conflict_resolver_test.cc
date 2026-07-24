// Copyright 2026 RethinkDB, all rights reserved.
#include "unittest/gtest.hpp"

#include "containers/uuid.hpp"
#include "rdb_protocol/cdc_types.hpp"
#include "rdb_protocol/conflict_resolver.hpp"
#include "rdb_protocol/datum.hpp"

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

// Helper: build a simple object datum from a map
datum_t make_obj(std::map<datum_string_t, datum_t> &&fields) {
    return datum_t(std::move(fields));
}

// Helper: build a string datum
datum_t make_str(const std::string &s) {
    return datum_t(datum_string_t(s));
}

// Helper: build a change_record_t with a serialized after_image
change_record_t make_record_with_image(
        change_operation_t op,
        const datum_t &after,
        const datum_t &before) {
    change_record_t r;
    r.op = op;
    r.commit_timestamp = 1000;
    r.event_id.table_id = generate_uuid();
    r.event_id.source_cluster_id = generate_uuid();
    r.event_id.shard_id = generate_uuid();
    r.event_id.lsn.value = 1;
    r.after_image = serialize_datum_to_vector(after);
    r.before_image = serialize_datum_to_vector(before);
    return r;
}

TEST(ConflictResolverTest, MergeDeleteReturnsNull) {
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);

    change_record_t del;
    del.op = change_operation_t::DELETE;
    del.commit_timestamp = 1000;
    del.event_id.table_id = generate_uuid();

    datum_t target = make_obj({});
    datum_t result = resolver.apply_merge(del, target, true);
    // DELETE returns an uninitialized (null) datum
    EXPECT_FALSE(result.has());
}

TEST(ConflictResolverTest, MergeInsertNewKey) {
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);

    std::map<datum_string_t, datum_t> after_fields;
    after_fields[datum_string_t("id")] = make_str("abc");
    after_fields[datum_string_t("val")] = datum_t(42.0);
    datum_t after_datum(std::move(after_fields));

    datum_t empty_before;  // uninitialized — INSERT has no before_image

    change_record_t ins = make_record_with_image(
        change_operation_t::INSERT, after_datum, empty_before);

    datum_t target;  // uninitialized — key doesn't exist yet
    datum_t result = resolver.apply_merge(ins, target, false);

    EXPECT_TRUE(result.has());
    EXPECT_EQ(result.get_type(), datum_t::R_OBJECT);
    EXPECT_EQ(result.obj_size(), 2u);
    EXPECT_EQ(result.get_field("val").as_num(), 42.0);
    // The "id" field should be a string "abc"
    EXPECT_EQ(result.get_field("id").get_type(), datum_t::R_STR);
}

TEST(ConflictResolverTest, MergeUpdateDeepMerge) {
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);

    // Target row: {"a": 1, "b": 2}
    std::map<datum_string_t, datum_t> target_fields;
    target_fields[datum_string_t("a")] = datum_t(1.0);
    target_fields[datum_string_t("b")] = datum_t(2.0);
    datum_t target(std::move(target_fields));

    // UPDATE after_image: {"b": 3, "c": 4} — overwrites "b", adds "c"
    std::map<datum_string_t, datum_t> update_fields;
    update_fields[datum_string_t("b")] = datum_t(3.0);
    update_fields[datum_string_t("c")] = datum_t(4.0);
    datum_t update_datum(std::move(update_fields));

    // Before image: could be anything for UPDATE
    datum_t before = make_obj({});

    change_record_t upd = make_record_with_image(
        change_operation_t::UPDATE, update_datum, before);

    datum_t result = resolver.apply_merge(upd, target, true);

    EXPECT_TRUE(result.has());
    EXPECT_EQ(result.get_type(), datum_t::R_OBJECT);
    // After merge: {"a": 1, "b": 3, "c": 4}
    EXPECT_EQ(result.obj_size(), 3u);
    EXPECT_EQ(result.get_field("a").as_num(), 1.0);  // target-only, survived
    EXPECT_EQ(result.get_field("b").as_num(), 3.0);  // source overwrote target
    EXPECT_EQ(result.get_field("c").as_num(), 4.0);  // source-only, added
}

TEST(ConflictResolverTest, MergeReplaceOverwrites) {
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);

    // Target row: {"old": 99, "keep": "data"}
    std::map<datum_string_t, datum_t> target_fields;
    target_fields[datum_string_t("old")] = datum_t(99.0);
    target_fields[datum_string_t("keep")] = make_str("data");
    datum_t target(std::move(target_fields));

    // REPLACE after_image: {"new": 1}
    std::map<datum_string_t, datum_t> replace_fields;
    replace_fields[datum_string_t("new")] = datum_t(1.0);
    datum_t replace_datum(std::move(replace_fields));

    datum_t before = make_obj({});

    change_record_t rep = make_record_with_image(
        change_operation_t::REPLACE, replace_datum, before);

    datum_t result = resolver.apply_merge(rep, target, true);

    EXPECT_TRUE(result.has());
    EXPECT_EQ(result.get_type(), datum_t::R_OBJECT);
    // Full replacement: target fields are gone, only replace fields survive
    EXPECT_EQ(result.obj_size(), 1u);
    EXPECT_EQ(result.get_field("new").as_num(), 1.0);
    // "old" and "keep" should NOT be present
    EXPECT_FALSE(result.get_field("old", NOTHROW).has());
    EXPECT_FALSE(result.get_field("keep", NOTHROW).has());
}

TEST(ConflictResolverTest, DetectConflictDiffOp) {
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);

    uuid_u table = generate_uuid();
    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();

    // Two records on the same row (same before_image), different ops
    change_record_t source;
    source.op = change_operation_t::UPDATE;
    source.commit_timestamp = 1000;
    source.event_id.table_id = table;
    source.event_id.source_cluster_id = cluster;
    source.event_id.shard_id = shard;
    source.event_id.lsn.value = 1;
    source.before_image = {'r', 'o', 'w', '1'};  // same row ID
    source.after_image = {'x'};

    change_record_t target;
    target.op = change_operation_t::DELETE;
    target.commit_timestamp = 2000;
    target.event_id.table_id = table;
    target.event_id.source_cluster_id = cluster;
    target.event_id.shard_id = shard;
    target.event_id.lsn.value = 2;
    target.before_image = {'r', 'o', 'w', '1'};  // same row ID
    target.after_image = {};  // DELETE has no after_image

    // Same PK + different op → conflict
    EXPECT_TRUE(resolver.detect_conflict(source, target));
}

TEST(ConflictResolverTest, NoConflictDiffPK) {
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);

    uuid_u table = generate_uuid();
    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();

    // Two records on different rows (different before_image)
    change_record_t source;
    source.op = change_operation_t::UPDATE;
    source.commit_timestamp = 1000;
    source.event_id.table_id = table;
    source.event_id.source_cluster_id = cluster;
    source.event_id.shard_id = shard;
    source.event_id.lsn.value = 1;
    source.before_image = {'r', 'o', 'w', '1'};  // row 1
    source.after_image = {'x'};

    change_record_t target;
    target.op = change_operation_t::UPDATE;
    target.commit_timestamp = 2000;
    target.event_id.table_id = table;
    target.event_id.source_cluster_id = cluster;
    target.event_id.shard_id = shard;
    target.event_id.lsn.value = 2;
    target.before_image = {'r', 'o', 'w', '2'};  // row 2
    target.after_image = {'y'};

    // Different PK → no conflict
    EXPECT_FALSE(resolver.detect_conflict(source, target));
}

}  // namespace unittest
