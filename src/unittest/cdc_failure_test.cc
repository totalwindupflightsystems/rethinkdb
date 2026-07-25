// Copyright 2026 RethinkDB, all rights reserved.
#include "unittest/gtest.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "concurrency/cond_var.hpp"
#include "containers/uuid.hpp"
#include "rdb_protocol/cdc_types.hpp"
#include "rdb_protocol/conflict_resolver.hpp"
#include "rdb_protocol/datum.hpp"
#include "time.hpp"

namespace unittest {
namespace {

using namespace ql;

// ── Helpers ──

change_record_t make_record(change_operation_t op, microtime_t ts,
                            uuid_u cluster, uuid_u shard, uint64_t lsn_val) {
    static uuid_u table = generate_uuid();
    change_record_t r;
    r.op = op;
    r.commit_timestamp = ts;
    r.event_id.source_cluster_id = cluster;
    r.event_id.table_id = table;
    r.event_id.shard_id = shard;
    r.event_id.lsn.value = lsn_val;
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

// Helper: true if uuid a < uuid b using string comparison
bool uuid_less(const uuid_u &a, const uuid_u &b) {
    return uuid_to_str(a) < uuid_to_str(b);
}

}  // namespace

// ── Conflict log corruption ──

TEST(CdcFailureTest, ConflictLogResolveNonexistentId) {
    // Resolving a UUID that doesn't exist in the log should be a no-op,
    // not a crash.
    conflict_log_t log;

    // Add one entry
    conflict_log_entry_t entry;
    entry.id = generate_uuid();
    entry.occurred_at = current_microtime();
    entry.action = operator_action_t::PENDING;
    log.record(entry);
    EXPECT_EQ(log.size(), 1u);

    // Resolve a non-existent ID → should not crash, size unchanged
    uuid_u nonexistent = generate_uuid();
    log.resolve(nonexistent, operator_action_t::RESOLVE, "nope");
    EXPECT_EQ(log.size(), 1u);
    EXPECT_EQ(log.pending_count(), 1u);
}

TEST(CdcFailureTest, ConflictLogEmptyGetPending) {
    // get_pending() on an empty log should return an empty vector
    conflict_log_t log;
    auto pending = log.get_pending();
    EXPECT_TRUE(pending.empty());
    EXPECT_EQ(log.pending_count(), 0u);
}

TEST(CdcFailureTest, ConflictLogGetAllEmpty) {
    conflict_log_t log;
    auto all = log.get_all();
    EXPECT_TRUE(all.empty());
}

TEST(CdcFailureTest, ConflictLogDoubleResolve) {
    // Resolving the same conflict twice should not crash
    conflict_log_t log;

    uuid_u id = generate_uuid();
    conflict_log_entry_t entry;
    entry.id = id;
    entry.occurred_at = current_microtime();
    entry.action = operator_action_t::PENDING;
    log.record(entry);

    log.resolve(id, operator_action_t::RESOLVE, "first");
    EXPECT_EQ(log.pending_count(), 0u);

    // Second resolve on same ID: no-op, should not crash
    log.resolve(id, operator_action_t::OVERRIDE, "second");
    EXPECT_EQ(log.pending_count(), 0u);

    auto all = log.get_all();
    ASSERT_EQ(all.size(), 1u);
    // The action should be updated to the second resolution
    EXPECT_EQ(all[0].action, operator_action_t::OVERRIDE);
    EXPECT_EQ(all[0].operator_note, "second");
}

TEST(CdcFailureTest, ConflictLogMassiveConcurrentWrites) {
    // Stress test: many threads writing simultaneously
    conflict_log_t log;
    const int num_threads = 8;
    const int entries_per_thread = 200;

    std::vector<std::thread> threads;
    std::atomic<int> ready(0);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&log, entries_per_thread, &ready]() {
            ++ready;
            // Block until all threads are created
            while (ready.load() < num_threads) {
                std::this_thread::yield();
            }
            for (int i = 0; i < entries_per_thread; ++i) {
                conflict_log_entry_t entry;
                entry.id = generate_uuid();
                entry.occurred_at = current_microtime();
                entry.action = operator_action_t::PENDING;
                log.record(entry);
            }
        });
    }

    for (auto &th : threads) { th.join(); }

    EXPECT_EQ(log.size(), num_threads * entries_per_thread);
    EXPECT_EQ(log.pending_count(), num_threads * entries_per_thread);
}

// ── Resolver edge cases ──

TEST(CdcFailureTest, ResolverEmptyBeforeImageConflict) {
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);

    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();

    // Two records with empty before_images (simulating INSERT vs INSERT)
    change_record_t source;
    source.op = change_operation_t::INSERT;
    source.commit_timestamp = 1000;
    source.event_id.source_cluster_id = cluster;
    source.event_id.table_id = generate_uuid();
    source.event_id.shard_id = shard;
    source.event_id.lsn.value = 1;
    source.after_image = {'a'};
    source.before_image = {};

    change_record_t target;
    target.op = change_operation_t::INSERT;
    target.commit_timestamp = 2000;
    target.event_id.source_cluster_id = cluster;
    target.event_id.table_id = source.event_id.table_id;
    target.event_id.shard_id = shard;
    target.event_id.lsn.value = 2;
    target.after_image = {'b'};
    target.before_image = {};

    // INSERT vs INSERT: source should win
    auto result = resolver.resolve(source, target);
    EXPECT_FALSE(result.skipped);
    EXPECT_EQ(result.resolved.op, change_operation_t::INSERT);
    EXPECT_EQ(result.resolved.event_id.lsn.value, 1u);
}

TEST(CdcFailureTest, ResolverEmptyAfterImageDelete) {
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);

    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();

    // DELETE has empty after_image
    change_record_t del = make_record(
        change_operation_t::DELETE, 1000, cluster, shard, 1);
    del.after_image = {};
    del.before_image = {};

    change_record_t upd = make_record(
        change_operation_t::UPDATE, 2000, cluster, shard, 2);

    auto result = resolver.resolve(del, upd);
    EXPECT_FALSE(result.skipped);
    // UPDATE is newer (2000 > 1000), so target wins
    EXPECT_EQ(result.resolved.commit_timestamp, 2000u);
}

TEST(CdcFailureTest, ResolverMaxMicrotimeValues) {
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);

    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();

    // Max representable microtime
    change_record_t source = make_record(
        change_operation_t::UPDATE, UINT64_MAX, cluster, shard, 1);
    change_record_t target = make_record(
        change_operation_t::UPDATE, UINT64_MAX / 2, cluster, shard, 2);

    auto result = resolver.resolve(source, target);
    EXPECT_FALSE(result.skipped);
    EXPECT_EQ(result.resolved.commit_timestamp, UINT64_MAX);
}

TEST(CdcFailureTest, ResolverZeroTimestamps) {
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);

    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();

    change_record_t source = make_record(
        change_operation_t::UPDATE, 0, cluster, shard, 1);
    change_record_t target = make_record(
        change_operation_t::UPDATE, 0, cluster, shard, 2);

    auto result = resolver.resolve(source, target);
    EXPECT_FALSE(result.skipped);
    // Same timestamp, LSN tiebreak: target has higher LSN (2 > 1)
    EXPECT_EQ(result.resolved.event_id.lsn.value, 2u);
}

TEST(CdcFailureTest, ResolverAllPoliciesWithInterruptor) {
    // Simulate interrupted resolution by using threads.
    // The resolver itself is stateless per-call, so we verify
    // each policy works correctly under concurrent threads.

    conflict_resolver_t lww(conflict_resolution_policy_t::LAST_WRITE_WINS);
    conflict_resolver_t sw(conflict_resolution_policy_t::SOURCE_WINS);
    conflict_resolver_t tw(conflict_resolution_policy_t::TARGET_WINS);
    conflict_resolver_t mnl(conflict_resolution_policy_t::MANUAL);

    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();
    change_record_t source = make_record(
        change_operation_t::UPDATE, 1000, cluster, shard, 1);
    change_record_t target = make_record(
        change_operation_t::UPDATE, 2000, cluster, shard, 2);

    // Run all policies concurrently from multiple threads
    std::atomic<int> errors(0);
    auto worker = [&](conflict_resolver_t *resolver) {
        for (int i = 0; i < 100; ++i) {
            auto result = resolver->resolve(source, target);
            if (result.skipped && resolver->policy()
                    != conflict_resolution_policy_t::MANUAL) {
                ++errors;
            }
        }
    };

    std::thread t1(worker, &lww);
    std::thread t2(worker, &sw);
    std::thread t3(worker, &tw);
    std::thread t4(worker, &mnl);
    t1.join(); t2.join(); t3.join(); t4.join();

    EXPECT_EQ(errors.load(), 0);
}

// ── Split-brain / network partition simulation ──

TEST(CdcFailureTest, SplitBrainConflictingWrites) {
    // Simulate split-brain: two clusters write conflicting records
    // for the same row independently.
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);

    uuid_u cluster_a = generate_uuid();
    uuid_u cluster_b = generate_uuid();
    uuid_u shard_a = generate_uuid();
    uuid_u shard_b = generate_uuid();

    // Both clusters write to the same row (same before_image / PK)
    change_record_t source;
    source.op = change_operation_t::UPDATE;
    source.commit_timestamp = 1000;
    source.event_id.source_cluster_id = cluster_a;
    source.event_id.table_id = generate_uuid();
    source.event_id.shard_id = shard_a;
    source.event_id.lsn.value = 1;
    source.before_image = {'r', 'o', 'w', 'X'};
    source.after_image = {'a'};

    change_record_t target;
    target.op = change_operation_t::UPDATE;
    target.commit_timestamp = 1000;  // same timestamp
    target.event_id.source_cluster_id = cluster_b;
    target.event_id.table_id = source.event_id.table_id;
    target.event_id.shard_id = shard_b;
    target.event_id.lsn.value = 1;
    target.before_image = {'r', 'o', 'w', 'X'};  // same PK
    target.after_image = {'b'};

    // detect_conflict should return true
    EXPECT_TRUE(resolver.detect_conflict(source, target));

    // LWW resolution: same timestamp, cluster_id tiebreak
    auto result = resolver.resolve(source, target);
    EXPECT_FALSE(result.skipped);
}

TEST(CdcFailureTest, SplitBrainDifferentClustersSameShard) {
    // Two clusters with different cluster IDs but same shard_id
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);

    uuid_u cluster_a = generate_uuid();
    uuid_u cluster_b = generate_uuid();
    uuid_u shard = generate_uuid();

    change_record_t source = make_record(
        change_operation_t::UPDATE, 5000, cluster_a, shard, 10);
    change_record_t target = make_record(
        change_operation_t::UPDATE, 5000, cluster_b, shard, 20);

    auto result = resolver.resolve(source, target);
    EXPECT_FALSE(result.skipped);
    // Same timestamp → cluster_id tiebreak, then shard (same), then LSN
    // Since shard is same, LSN decides → target has higher LSN (20 > 10)
    EXPECT_EQ(result.resolved.event_id.lsn.value, 20u);
}

// ── Resolver timeout/deadline exceeded ──

TEST(CdcFailureTest, ResolverMassiveConflictLogStress) {
    // Test that the conflict log can handle rapid-fire resolutions
    // of a large number of entries without deadlock.
    conflict_log_t log;

    const int num_entries = 500;
    std::vector<uuid_u> ids;
    ids.reserve(num_entries);

    for (int i = 0; i < num_entries; ++i) {
        conflict_log_entry_t entry;
        entry.id = generate_uuid();
        entry.occurred_at = current_microtime();
        entry.action = operator_action_t::PENDING;
        ids.push_back(entry.id);
        log.record(entry);
    }
    EXPECT_EQ(log.size(), num_entries);

    // Resolve all entries
    for (const auto &id : ids) {
        log.resolve(id, operator_action_t::SKIP, "");
    }
    EXPECT_EQ(log.pending_count(), 0u);
    EXPECT_EQ(log.size(), num_entries);
}

// ── Recovery scenarios ──

TEST(CdcFailureTest, ResumeAfterCrashLogReplay) {
    // Simulate: conflict was logged → crash → restart → resolve using log.
    conflict_log_t log;

    uuid_u id = generate_uuid();
    conflict_log_entry_t entry;
    entry.id = id;
    entry.occurred_at = current_microtime();
    entry.action = operator_action_t::PENDING;
    // Set up source/target records to simulate a real conflict
    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();
    entry.source = make_record(change_operation_t::UPDATE, 1000, cluster, shard, 1);
    entry.target = make_record(change_operation_t::UPDATE, 2000, cluster, shard, 2);
    log.record(entry);

    // Simulate crash and restart by re-reading the log
    auto pending = log.get_pending();
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending[0].id, id);
    EXPECT_EQ(pending[0].action, operator_action_t::PENDING);

    // Operator resolves after restart
    log.resolve(pending[0].id, operator_action_t::RESOLVE, "recovered after crash");
    EXPECT_EQ(log.pending_count(), 0u);
}

TEST(CdcFailureTest, PartialWriteRecoveryMetadata) {
    // Simulate a crash where a conflict was detected but metadata
    // was only partially written: entry in log but resolution not applied.
    conflict_log_t log;

    // Simulate: conflict raised, entry logged, operator resolved → crash
    uuid_u id = generate_uuid();
    conflict_log_entry_t entry;
    entry.id = id;
    entry.occurred_at = current_microtime();
    entry.action = operator_action_t::PENDING;
    log.record(entry);

    EXPECT_EQ(log.pending_count(), 1u);

    // Operator resolves
    log.resolve(id, operator_action_t::OVERRIDE, "manual override");

    // After resolution, pending should be 0 even if crash occurs later
    EXPECT_EQ(log.pending_count(), 0u);

    // The resolved entry should still be in get_all()
    auto all = log.get_all();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].action, operator_action_t::OVERRIDE);
}

// ── Boundary conditions ──

TEST(CdcFailureTest, InvalidHandlerConfigRestrictedPolicy) {
    conflict_resolver_t resolver(conflict_resolution_policy_t::CUSTOM_HANDLER);
    resolver.set_safety_level(handler_safety_level_t::RESTRICTED);

    // Setting a handler at RESTRICTED level should throw
    EXPECT_ANY_THROW(
        resolver.set_custom_handler(
            [](const change_record_t &s, const change_record_t &)
                -> conflict_resolution_result_t {
                conflict_resolution_result_t r;
                r.resolved = s;
                return r;
            }));
}

TEST(CdcFailureTest, ConflictDetectionSameOpSameImageNoConflict) {
    // Verify idempotent writes are not conflicts
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);

    uuid_u table = generate_uuid();
    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();

    change_record_t source;
    source.op = change_operation_t::UPDATE;
    source.commit_timestamp = 1000;
    source.event_id.table_id = table;
    source.event_id.source_cluster_id = cluster;
    source.event_id.shard_id = shard;
    source.event_id.lsn.value = 1;
    source.before_image = {'r', 'o', 'w'};
    source.after_image = {'d', 'a', 't', 'a'};

    change_record_t target;
    target.op = change_operation_t::UPDATE;
    target.commit_timestamp = 2000;
    target.event_id.table_id = table;
    target.event_id.source_cluster_id = cluster;
    target.event_id.shard_id = shard;
    target.event_id.lsn.value = 2;
    target.before_image = {'r', 'o', 'w'};
    target.after_image = {'d', 'a', 't', 'a'};  // same after_image

    // Same PK, same op, same after_image → no conflict (idempotent)
    EXPECT_FALSE(resolver.detect_conflict(source, target));
}

TEST(CdcFailureTest, ConflictDetectionDifferentTableNoConflict) {
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);

    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();

    change_record_t source;
    source.op = change_operation_t::UPDATE;
    source.commit_timestamp = 1000;
    source.event_id.table_id = generate_uuid();  // table A
    source.event_id.source_cluster_id = cluster;
    source.event_id.shard_id = shard;
    source.event_id.lsn.value = 1;
    source.before_image = {'r', 'o', 'w'};
    source.after_image = {'a'};

    change_record_t target;
    target.op = change_operation_t::UPDATE;
    target.commit_timestamp = 2000;
    target.event_id.table_id = generate_uuid();  // table B (different!)
    target.event_id.source_cluster_id = cluster;
    target.event_id.shard_id = shard;
    target.event_id.lsn.value = 2;
    target.before_image = {'r', 'o', 'w'};  // same PK but different table
    target.after_image = {'b'};

    // Different table → no conflict regardless of PK
    EXPECT_FALSE(resolver.detect_conflict(source, target));
}

// ── Concurrent resolver init ──

TEST(CdcFailureTest, ConcurrentResolverInit) {
    // Multiple threads constructing resolvers simultaneously
    std::atomic<int> ready(0);
    std::atomic<int> errors(0);

    auto worker = [&]() {
        ++ready;
        while (ready.load() < 4) { std::this_thread::yield(); }
        for (int i = 0; i < 50; ++i) {
            conflict_resolver_t lww(conflict_resolution_policy_t::LAST_WRITE_WINS);
            conflict_resolver_t sw(conflict_resolution_policy_t::SOURCE_WINS);
            conflict_resolver_t tw(conflict_resolution_policy_t::TARGET_WINS);
            conflict_resolver_t mnl(conflict_resolution_policy_t::MANUAL);
            conflict_resolver_t cust(conflict_resolution_policy_t::CUSTOM_HANDLER);

            uuid_u cluster = generate_uuid();
            uuid_u shard = generate_uuid();
            auto s = make_record(change_operation_t::UPDATE, 1000, cluster, shard, 1);
            auto t = make_record(change_operation_t::UPDATE, 2000, cluster, shard, 2);

            // All should resolve without crashing
            try {
                lww.resolve(s, t);
                sw.resolve(s, t);
                tw.resolve(s, t);
                mnl.resolve(s, t);
                cust.resolve(s, t);
            } catch (...) { ++errors; }
        }
    };

    std::thread t1(worker), t2(worker), t3(worker), t4(worker);
    t1.join(); t2.join(); t3.join(); t4.join();
    EXPECT_EQ(errors.load(), 0);
}

// ── Corrupt / malformed conflict metadata ──

TEST(CdcFailureTest, ApplyMergeEmptyImages) {
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);

    // INSERT with empty after_image: deserialize yields uninitialized datum
    change_record_t ins;
    ins.op = change_operation_t::INSERT;
    ins.after_image = {};
    ins.before_image = {};
    ins.commit_timestamp = 1000;
    ins.event_id.table_id = generate_uuid();

    datum_t target;  // uninitialized
    datum_t result = resolver.apply_merge(ins, target, false);
    // Empty after_image yields uninitialized datum → NOT has()
    EXPECT_FALSE(result.has());
}

TEST(CdcFailureTest, ApplyMergeDeleteWithEmptyTarget) {
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);

    change_record_t del;
    del.op = change_operation_t::DELETE;
    del.commit_timestamp = 1000;
    del.event_id.table_id = generate_uuid();

    // target doesn't exist → DELETE still returns null
    datum_t result = resolver.apply_merge(del, datum_t(), false);
    EXPECT_FALSE(result.has());
}

TEST(CdcFailureTest, TombstoneDetection) {
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);

    // Build a record with _cdc_tombstone in before_image
    std::map<datum_string_t, datum_t> fields;
    fields[datum_string_t("_cdc_tombstone")] = datum_t(datum_string_t("2024-01-01T00:00:00Z"));
    fields[datum_string_t("data")] = datum_t(42.0);
    datum_t tombstone_datum(std::move(fields));

    change_record_t upd;
    upd.op = change_operation_t::UPDATE;
    upd.commit_timestamp = 1000;
    upd.event_id.table_id = generate_uuid();
    upd.event_id.source_cluster_id = generate_uuid();
    upd.event_id.shard_id = generate_uuid();
    upd.event_id.lsn.value = 1;
    upd.before_image = serialize_datum_to_vector(tombstone_datum);
    upd.after_image = {'x'};

    datum_t target;
    std::map<datum_string_t, datum_t> target_fields;
    target_fields[datum_string_t("data")] = datum_t(99.0);
    target = datum_t(std::move(target_fields));

    datum_t result = resolver.apply_merge(upd, target, true);
    // Tombstone in before_image → treat as DELETE → null
    EXPECT_FALSE(result.has());
}

// ── LWW comprehensive tiebreaking ──

TEST(CdcFailureTest, LwwTiebreakSourceClusterId) {
    // Same timestamp, different cluster_id: compare UUID strings
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);

    uuid_u cluster_a = generate_uuid();
    uuid_u cluster_b = generate_uuid();
    // Ensure deterministic ordering by UUID string comparison
    if (uuid_less(cluster_b, cluster_a)) { std::swap(cluster_a, cluster_b); }
    uuid_u shard = generate_uuid();

    change_record_t source = make_record(
        change_operation_t::UPDATE, 5000, cluster_a, shard, 1);
    change_record_t target = make_record(
        change_operation_t::UPDATE, 5000, cluster_b, shard, 1);

    auto result = resolver.resolve(source, target);
    EXPECT_FALSE(result.skipped);
    // cluster_b > cluster_a per UUID ordering → target wins
    EXPECT_EQ(result.resolved.event_id.source_cluster_id, cluster_b);
}

TEST(CdcFailureTest, LwwTiebreakShardId) {
    // Same timestamp, same cluster, different shard_id
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);

    uuid_u cluster = generate_uuid();
    uuid_u shard_a = generate_uuid();
    uuid_u shard_b = generate_uuid();
    if (uuid_less(shard_b, shard_a)) { std::swap(shard_a, shard_b); }

    change_record_t source = make_record(
        change_operation_t::UPDATE, 5000, cluster, shard_a, 1);
    change_record_t target = make_record(
        change_operation_t::UPDATE, 5000, cluster, shard_b, 1);

    auto result = resolver.resolve(source, target);
    EXPECT_FALSE(result.skipped);
    // shard_b > shard_a → target wins
    EXPECT_EQ(result.resolved.event_id.shard_id, shard_b);
}

TEST(CdcFailureTest, LwwTiebreakLsn) {
    // Same timestamp, cluster, shard → LSN tiebreak
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);

    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();

    change_record_t source = make_record(
        change_operation_t::UPDATE, 5000, cluster, shard, 1);
    change_record_t target = make_record(
        change_operation_t::UPDATE, 5000, cluster, shard, 2);

    auto result = resolver.resolve(source, target);
    EXPECT_FALSE(result.skipped);
    // target has higher LSN (2 > 1)
    EXPECT_EQ(result.resolved.event_id.lsn.value, 2u);
}

}  // namespace unittest
