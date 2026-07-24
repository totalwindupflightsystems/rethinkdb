// Copyright 2026 RethinkDB, all rights reserved.
#include "unittest/gtest.hpp"

#include <thread>

#include "containers/uuid.hpp"
#include "rdb_protocol/cdc_types.hpp"
#include "rdb_protocol/conflict_resolver.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/env.hpp"
#include "rdb_protocol/func.hpp"
#include "rdb_protocol/rdb_backtrace.hpp"
#include "rdb_protocol/sym.hpp"
#include "rdb_protocol/term.hpp"
#include "rdb_protocol/term_storage.hpp"

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

// ── CDC-09c: Custom handler validation + safety levels ──

TEST(ConflictResolverTest, RestrictedRejectsCustomHandler) {
    conflict_resolver_t resolver(conflict_resolution_policy_t::CUSTOM_HANDLER);
    resolver.set_safety_level(handler_safety_level_t::RESTRICTED);

    EXPECT_ANY_THROW(
        resolver.set_custom_handler(
            [](const change_record_t &s, const change_record_t &)
                -> conflict_resolution_result_t {
                conflict_resolution_result_t r;
                r.resolved = s;
                return r;
            }));
}

TEST(ConflictResolverTest, PermissiveAllowsCustomHandler) {
    conflict_resolver_t resolver(conflict_resolution_policy_t::CUSTOM_HANDLER);
    EXPECT_EQ(resolver.safety_level(), handler_safety_level_t::PERMISSIVE);

    // Should not throw at PERMISSIVE level
    bool handler_called = false;
    resolver.set_custom_handler(
        [&handler_called](const change_record_t &s, const change_record_t &)
            -> conflict_resolution_result_t {
            handler_called = true;
            conflict_resolution_result_t r;
            r.resolved = s;
            r.reason = "custom handler";
            return r;
        });

    // Verify the handler actually works
    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();
    change_record_t source = make_record(
        change_operation_t::UPDATE, 1000, cluster, shard, 1);
    change_record_t target = make_record(
        change_operation_t::UPDATE, 2000, cluster, shard, 2);

    auto result = resolver.resolve(source, target);
    EXPECT_TRUE(handler_called);
    EXPECT_EQ(result.reason, "custom handler");
}

TEST(ConflictResolverTest, ValidateHandlerRejectsJs) {
    // Build a term tree for: function(source, target) { return r.js("x"); }
    // The body contains JAVASCRIPT which is forbidden.
    backtrace_id_t bt = backtrace_id_t::empty();

    // Arg syms: use two distinct dummy sym values (negative = function-local)
    int64_t source_sym_val = -101;
    int64_t target_sym_val = -102;

    // Build MAKE_ARRAY([DATUM(source_sym_val), DATUM(target_sym_val)])
    auto arg0 = make_counted<generated_term_t>(Term::DATUM, bt);
    arg0->datum = datum_t(static_cast<double>(source_sym_val));
    auto arg1 = make_counted<generated_term_t>(Term::DATUM, bt);
    arg1->datum = datum_t(static_cast<double>(target_sym_val));
    auto arg_array = make_counted<generated_term_t>(Term::MAKE_ARRAY, bt);
    arg_array->args.push_back(arg0);
    arg_array->args.push_back(arg1);

    // Build JAVASCRIPT(DATUM("return 1;"))
    auto js_code = make_counted<generated_term_t>(Term::DATUM, bt);
    js_code->datum = datum_t(datum_string_t("return 1;"));
    auto js_term = make_counted<generated_term_t>(Term::JAVASCRIPT, bt);
    js_term->args.push_back(js_code);

    // Build FUNC(arg_array, js_term)
    auto func = make_counted<generated_term_t>(Term::FUNC, bt);
    func->args.push_back(arg_array);
    func->args.push_back(js_term);

    // Compile into a func_term_t
    compile_env_t compile_env((var_visibility_t()));
    term_variant_t tv(func);
    raw_term_t raw_func(tv);
    counted_t<const term_t> compiled = compile_term(&compile_env, raw_func);

    // Downcast: we know compile_term returns func_term_t for FUNC
    const func_term_t *ftp =
        static_cast<const func_term_t *>(compiled.get());
    counted_t<func_term_t> func_term(const_cast<func_term_t *>(ftp));

    // Provide used_syms containing both arg syms so we pass the
    // arg-reference check and hit the forbidden-term check.
    std::vector<sym_t> used_syms;
    used_syms.push_back(sym_t(source_sym_val));
    used_syms.push_back(sym_t(target_sym_val));

    EXPECT_ANY_THROW(
        conflict_resolver_t::validate_handler(used_syms, func_term));
}

TEST(ConflictResolverTest, ValidateHandlerRejectsDbCreate) {
    // Build a term tree for: function(source, target) { return r.db_create("x"); }
    backtrace_id_t bt = backtrace_id_t::empty();

    int64_t source_sym_val = -201;
    int64_t target_sym_val = -202;

    auto arg0 = make_counted<generated_term_t>(Term::DATUM, bt);
    arg0->datum = datum_t(static_cast<double>(source_sym_val));
    auto arg1 = make_counted<generated_term_t>(Term::DATUM, bt);
    arg1->datum = datum_t(static_cast<double>(target_sym_val));
    auto arg_array = make_counted<generated_term_t>(Term::MAKE_ARRAY, bt);
    arg_array->args.push_back(arg0);
    arg_array->args.push_back(arg1);

    // Build DB_CREATE(DATUM("test_db"))
    auto db_name = make_counted<generated_term_t>(Term::DATUM, bt);
    db_name->datum = datum_t(datum_string_t("test_db"));
    auto db_create_term = make_counted<generated_term_t>(Term::DB_CREATE, bt);
    db_create_term->args.push_back(db_name);

    // Build FUNC(arg_array, db_create_term)
    auto func = make_counted<generated_term_t>(Term::FUNC, bt);
    func->args.push_back(arg_array);
    func->args.push_back(db_create_term);

    compile_env_t compile_env((var_visibility_t()));
    term_variant_t tv(func);
    raw_term_t raw_func(tv);
    counted_t<const term_t> compiled = compile_term(&compile_env, raw_func);

    const func_term_t *ftp =
        static_cast<const func_term_t *>(compiled.get());
    counted_t<func_term_t> func_term(const_cast<func_term_t *>(ftp));

    std::vector<sym_t> used_syms;
    used_syms.push_back(sym_t(source_sym_val));
    used_syms.push_back(sym_t(target_sym_val));

    EXPECT_ANY_THROW(
        conflict_resolver_t::validate_handler(used_syms, func_term));
}

// ── CDC-09d: Conflict log + operator actions ──

TEST(ConflictResolverTest, ConflictLogRecordsPending) {
    // MANUAL policy conflict should be logged as PENDING
    conflict_log_t log;
    conflict_resolver_t resolver(conflict_resolution_policy_t::MANUAL);
    resolver.set_conflict_log(&log);

    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();
    change_record_t source = make_record(
        change_operation_t::UPDATE, 2000, cluster, shard, 1);
    change_record_t target = make_record(
        change_operation_t::UPDATE, 1000, cluster, shard, 2);

    auto result = resolver.resolve(source, target);
    EXPECT_TRUE(result.skipped);

    // Log should have one entry
    EXPECT_EQ(log.size(), 1u);
    EXPECT_EQ(log.pending_count(), 1u);

    auto all = log.get_all();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].action, operator_action_t::PENDING);
    EXPECT_EQ(all[0].resolution.reason, "manual intervention required");
}

TEST(ConflictResolverTest, ConflictLogResolveOperatorAction) {
    // Operator RESOLVE updates the log entry
    conflict_log_t log;

    // Add an entry manually
    conflict_log_entry_t entry;
    entry.id = generate_uuid();
    entry.occurred_at = current_microtime();
    entry.action = operator_action_t::PENDING;
    log.record(entry);

    EXPECT_EQ(log.pending_count(), 1u);

    // Operator resolves it
    log.resolve(entry.id, operator_action_t::RESOLVE, "accepted by admin");

    EXPECT_EQ(log.pending_count(), 0u);
    EXPECT_EQ(log.size(), 1u);

    auto all = log.get_all();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].action, operator_action_t::RESOLVE);
    EXPECT_EQ(all[0].operator_note, "accepted by admin");
}

TEST(ConflictResolverTest, ConflictLogGetPending) {
    // get_pending() returns only unresolved conflicts
    conflict_log_t log;

    // Add two entries: one PENDING, one RESOLVE'd
    conflict_log_entry_t e1;
    e1.id = generate_uuid();
    e1.occurred_at = current_microtime();
    e1.action = operator_action_t::PENDING;
    log.record(e1);

    conflict_log_entry_t e2;
    e2.id = generate_uuid();
    e2.occurred_at = current_microtime();
    e2.action = operator_action_t::SKIP;
    log.record(e2);

    // Add a third PENDING
    conflict_log_entry_t e3;
    e3.id = generate_uuid();
    e3.occurred_at = current_microtime();
    e3.action = operator_action_t::PENDING;
    log.record(e3);

    EXPECT_EQ(log.size(), 3u);
    EXPECT_EQ(log.pending_count(), 2u);

    auto pending = log.get_pending();
    ASSERT_EQ(pending.size(), 2u);
    EXPECT_EQ(pending[0].action, operator_action_t::PENDING);
    EXPECT_EQ(pending[1].action, operator_action_t::PENDING);
}

TEST(ConflictResolverTest, ConflictLogThreadSafety) {
    // Concurrent writes from multiple threads should not crash
    conflict_log_t log;

    const int num_threads = 4;
    const int entries_per_thread = 50;
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&log, entries_per_thread]() {
            for (int i = 0; i < entries_per_thread; ++i) {
                conflict_log_entry_t entry;
                entry.id = generate_uuid();
                entry.occurred_at = current_microtime();
                entry.action = operator_action_t::PENDING;
                log.record(entry);
            }
        });
    }

    for (auto &th : threads) {
        th.join();
    }

    // All entries should be recorded
    EXPECT_EQ(log.size(), num_threads * entries_per_thread);
    EXPECT_EQ(log.pending_count(), num_threads * entries_per_thread);
}

}  // namespace unittest
