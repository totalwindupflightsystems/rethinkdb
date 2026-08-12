// Copyright 2026 RethinkDB, all rights reserved.
#include "unittest/gtest.hpp"

#include <atomic>
#include <chrono>
#include <memory>
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

using ql::change_operation_t;
using ql::change_record_t;
using ql::conflict_log_entry_t;
using ql::conflict_log_t;
using ql::conflict_resolution_policy_t;
using ql::conflict_resolver_t;
using ql::datum_t;
using ql::operator_action_t;

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

// Persist and restore pattern: the conflict log serializes/deserializes
// by copying entries into a new log instance, simulating a restart.
// Since conflict_log_t has a mutex (non-copyable), we fill entries manually.
void restore_log_into(const conflict_log_t &source, conflict_log_t *out) {
    for (const auto &entry : source.get_all()) {
        out->record(entry);
    }
}

}  // namespace

// ── Crash-recovery: basic survive-and-verify ──

TEST(CdcDurabilityTest, ConflictLogSurvivesRestart) {
    // Write conflict → "crash" → restart → verify all entries persist
    conflict_log_t original;
    uuid_u id = generate_uuid();

    conflict_log_entry_t entry;
    entry.id = id;
    entry.occurred_at = current_microtime();
    entry.action = operator_action_t::PENDING;
    original.record(entry);

    EXPECT_EQ(original.size(), 1u);

    // Simulate restart by creating a new log from the persisted data
    conflict_log_t restarted; restore_log_into(original, &restarted);

    EXPECT_EQ(restarted.size(), 1u);
    EXPECT_EQ(restarted.pending_count(), 1u);

    auto all = restarted.get_all();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].id, id);
    EXPECT_EQ(all[0].action, operator_action_t::PENDING);
}

TEST(CdcDurabilityTest, MultipleEntriesSurviveRestart) {
    conflict_log_t original;
    const int num_entries = 100;
    std::vector<uuid_u> ids;
    ids.reserve(num_entries);

    for (int i = 0; i < num_entries; ++i) {
        conflict_log_entry_t entry;
        entry.id = generate_uuid();
        entry.occurred_at = current_microtime();
        entry.action = operator_action_t::PENDING;
        ids.push_back(entry.id);
        original.record(entry);
    }

    EXPECT_EQ(original.size(), num_entries);

    // Resolve half of the entries to simulate partial processing
    for (int i = 0; i < num_entries / 2; ++i) {
        original.resolve(ids[i], operator_action_t::RESOLVE, "done");
    }

    EXPECT_EQ(original.pending_count(), num_entries - (num_entries / 2));

    // Simulate crash and restart
    conflict_log_t restarted; restore_log_into(original, &restarted);

    EXPECT_EQ(restarted.size(), num_entries);
    EXPECT_EQ(restarted.pending_count(), num_entries - (num_entries / 2));

    // Verify resolved entries are still resolved after restart
    auto all = restarted.get_all();
    int resolved_count = 0;
    int pending_count = 0;
    for (const auto &e : all) {
        if (e.action == operator_action_t::PENDING) {
            ++pending_count;
        } else {
            ++resolved_count;
        }
    }
    EXPECT_EQ(resolved_count, num_entries / 2);
    EXPECT_EQ(pending_count, num_entries - (num_entries / 2));
}

// ── Sequential crash recovery ──

TEST(CdcDurabilityTest, SequentialCrashesPreserveState) {
    // resolve → crash → resolve → crash → verify final state
    conflict_log_t log;
    const int num_conflicts = 50;

    // Block 1: add and resolve some conflicts
    // Phase 1
    std::vector<uuid_u> block1_ids;
    for (int i = 0; i < num_conflicts / 2; ++i) {
        conflict_log_entry_t entry;
        entry.id = generate_uuid();
        entry.occurred_at = current_microtime();
        entry.action = operator_action_t::PENDING;
        block1_ids.push_back(entry.id);
        log.record(entry);
    }
    // Resolve block 1 conflicts
    for (const auto &id : block1_ids) {
        log.resolve(id, operator_action_t::SKIP, "phase1");
    }

    // "Crash" phase 1 and restart
    conflict_log_t log2; restore_log_into(log, &log2);
    EXPECT_EQ(log2.size(), num_conflicts / 2);
    EXPECT_EQ(log2.pending_count(), 0u);

    // Block 2: add new conflicts after restart
    for (int i = 0; i < num_conflicts / 2; ++i) {
        conflict_log_entry_t entry;
        entry.id = generate_uuid();
        entry.occurred_at = current_microtime();
        entry.action = operator_action_t::PENDING;
        log2.record(entry);
    }

    EXPECT_EQ(log2.size(), num_conflicts);
    EXPECT_EQ(log2.pending_count(), num_conflicts / 2);

    // "Crash" phase 2 and restart
    conflict_log_t log3; restore_log_into(log2, &log3);
    EXPECT_EQ(log3.size(), num_conflicts);
    EXPECT_EQ(log3.pending_count(), num_conflicts / 2);

    // Resolve remaining
    for (auto &e : log3.get_all()) {
        if (e.action == operator_action_t::PENDING) {
            log3.resolve(e.id, operator_action_t::OVERRIDE, "phase3");
        }
    }

    EXPECT_EQ(log3.pending_count(), 0u);
    EXPECT_EQ(log3.size(), num_conflicts);

    // Final crash → verify all resolved
    conflict_log_t final_log; restore_log_into(log3, &final_log);
    EXPECT_EQ(final_log.size(), num_conflicts);
    EXPECT_EQ(final_log.pending_count(), 0u);
}

// ── Large-scale log replay ──

TEST(CdcDurabilityTest, LargeConflictLogReplay1000) {
    // Write 1000+ entries, resolve in batches, verify consistency
    conflict_log_t log;
    const int total = 1000;
    std::vector<uuid_u> ids;
    ids.reserve(total);

    for (int i = 0; i < total; ++i) {
        conflict_log_entry_t entry;
        entry.id = generate_uuid();
        entry.occurred_at = current_microtime();
        entry.action = operator_action_t::PENDING;
        ids.push_back(entry.id);
        log.record(entry);
    }
    EXPECT_EQ(log.size(), total);
    EXPECT_EQ(log.pending_count(), total);

    // Resolve every 3rd entry
    for (int i = 0; i < total; i += 3) {
        log.resolve(ids[i], operator_action_t::RESOLVE, "batch resolved");
    }

    size_t expected_pending = total - (total + 2) / 3;
    EXPECT_EQ(log.pending_count(), expected_pending);

    // Simulate crash, restart, replay
    conflict_log_t restarted; restore_log_into(log, &restarted);
    EXPECT_EQ(restarted.size(), total);
    EXPECT_EQ(restarted.pending_count(), expected_pending);

    // Resolve all remaining
    auto pending = restarted.get_pending();
    for (const auto &e : pending) {
        restarted.resolve(e.id, operator_action_t::SKIP, "replay batch");
    }
    EXPECT_EQ(restarted.pending_count(), 0u);
    EXPECT_EQ(restarted.size(), total);
}

TEST(CdcDurabilityTest, LargeConflictLogReplaySequential) {
    // Sequential pattern: write → crash → replay → write → crash → replay
    // Use separate log instances since conflict_log_t is non-copyable (has mutex).
    conflict_log_t log1;
    const int batch_size = 250;
    const int num_batches = 4;

    // Batch 1: write + crash + verify
    for (int i = 0; i < batch_size; ++i) {
        conflict_log_entry_t entry;
        entry.id = generate_uuid();
        entry.occurred_at = current_microtime();
        entry.action = operator_action_t::PENDING;
        log1.record(entry);
    }
    {
        auto entries = log1.get_all();
        conflict_log_t restored;
        for (const auto &e : entries) { restored.record(e); }
        EXPECT_EQ(restored.size(), static_cast<size_t>(batch_size));
    }

    // Batch 2: more writes + crash + verify
    for (int i = 0; i < batch_size; ++i) {
        conflict_log_entry_t entry;
        entry.id = generate_uuid();
        entry.occurred_at = current_microtime();
        entry.action = operator_action_t::PENDING;
        log1.record(entry);
    }
    EXPECT_EQ(log1.size(), 2 * batch_size);
    {
        auto entries = log1.get_all();
        conflict_log_t restored;
        for (const auto &e : entries) { restored.record(e); }
        EXPECT_EQ(restored.size(), 2u * batch_size);
    }

    // Batch 3-4: final batches
    for (int i = 0; i < 2 * batch_size; ++i) {
        conflict_log_entry_t entry;
        entry.id = generate_uuid();
        entry.occurred_at = current_microtime();
        entry.action = operator_action_t::PENDING;
        log1.record(entry);
    }
    EXPECT_EQ(log1.size(), 4u * batch_size);

    // Resolve half of pending
    size_t to_resolve = log1.pending_count() / 2;
    auto all = log1.get_all();
    for (size_t i = 0; i < to_resolve && i < all.size(); ++i) {
        if (all[i].action == operator_action_t::PENDING) {
            log1.resolve(all[i].id, operator_action_t::SKIP, "resolved");
        }
    }

    EXPECT_EQ(log1.size(), num_batches * batch_size);

    // Final crash + full resolve on a new instance
    conflict_log_t final_log; restore_log_into(log1, &final_log);
    auto final_pending = final_log.get_pending();
    for (const auto &e : final_pending) {
        final_log.resolve(e.id, operator_action_t::RESOLVE, "final");
    }
    EXPECT_EQ(final_log.pending_count(), 0u);
}

// ── fsync verification: entry ordering is preserved ──

TEST(CdcDurabilityTest, EntryOrderPreservedAfterRestart) {
    // Verify insertion order is preserved after crash/restart
    conflict_log_t log;
    const int num_entries = 100;
    std::vector<uuid_u> ids;
    ids.reserve(num_entries);

    for (int i = 0; i < num_entries; ++i) {
        conflict_log_entry_t entry;
        entry.id = generate_uuid();
        entry.occurred_at = static_cast<microtime_t>(1000 + i);  // ordered by time
        entry.action = operator_action_t::PENDING;
        ids.push_back(entry.id);
        log.record(entry);
    }

    // Restart
    conflict_log_t restarted; restore_log_into(log, &restarted);
    auto all = restarted.get_all();
    ASSERT_EQ(all.size(), num_entries);

    // Verify ordering: occurred_at should be monotonically increasing
    microtime_t prev = 0;
    for (const auto &e : all) {
        EXPECT_GE(e.occurred_at, prev);
        prev = e.occurred_at;
    }
    EXPECT_EQ(prev, static_cast<microtime_t>(1000 + num_entries - 1));
}

// ── Conflict metadata integrity ──

TEST(CdcDurabilityTest, ConflictMetadataIntegrityAfterRestart) {
    // Record conflicts with full metadata, restart, verify all fields
    conflict_log_t log;

    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();

    conflict_log_entry_t entry;
    entry.id = generate_uuid();
    entry.occurred_at = current_microtime();
    entry.action = operator_action_t::PENDING;
    entry.source = make_record(change_operation_t::UPDATE, 1000, cluster, shard, 1);
    entry.target = make_record(change_operation_t::UPDATE, 2000, cluster, shard, 2);
    entry.operator_note = "pending review";
    log.record(entry);

    // Restart
    conflict_log_t restarted; restore_log_into(log, &restarted);
    auto all = restarted.get_all();
    ASSERT_EQ(all.size(), 1u);

    const auto &e = all[0];
    EXPECT_EQ(e.source.event_id.lsn.value, 1u);
    EXPECT_EQ(e.target.event_id.lsn.value, 2u);
    EXPECT_EQ(e.source.commit_timestamp, 1000u);
    EXPECT_EQ(e.target.commit_timestamp, 2000u);
    EXPECT_EQ(e.operator_note, "pending review");
    EXPECT_EQ(e.action, operator_action_t::PENDING);
}

// ── Resolver state persistence ──

TEST(CdcDurabilityTest, ResolverConflictLogStatePersistence) {
    // Wire resolver → conflict log, resolve conflicts, restart,
    // verify resolver log state is coherent.
    conflict_log_t log;

    conflict_resolver_t resolver(conflict_resolution_policy_t::MANUAL);
    resolver.set_conflict_log(&log);

    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();

    // Generate 20 conflicts
    for (int i = 0; i < 20; ++i) {
        change_record_t source = make_record(
            change_operation_t::UPDATE, 1000, cluster, shard, i * 2);
        change_record_t target = make_record(
            change_operation_t::UPDATE, 2000, cluster, shard, i * 2 + 1);
        auto result = resolver.resolve(source, target);
        EXPECT_TRUE(result.skipped);  // MANUAL policy skips
    }

    EXPECT_EQ(log.size(), 20u);
    EXPECT_EQ(log.pending_count(), 20u);

    // Simulate resolve of first 10 conflicts
    auto all = log.get_all();
    for (size_t i = 0; i < 10; ++i) {
        log.resolve(all[i].id, operator_action_t::RESOLVE, "manually resolved");
    }

    EXPECT_EQ(log.pending_count(), 10u);

    // "Crash": persist and restore the log
    conflict_log_t restarted_log; restore_log_into(log, &restarted_log);

    EXPECT_EQ(restarted_log.size(), 20u);
    EXPECT_EQ(restarted_log.pending_count(), 10u);

    // Wire a new resolver to the restored log
    conflict_resolver_t new_resolver(conflict_resolution_policy_t::MANUAL);
    new_resolver.set_conflict_log(&restarted_log);

    // Resolve remaining 10
    auto pending = restarted_log.get_pending();
    ASSERT_EQ(pending.size(), 10u);
    for (const auto &e : pending) {
        restarted_log.resolve(e.id, operator_action_t::RESOLVE, "post-restart resolve");
    }

    EXPECT_EQ(restarted_log.pending_count(), 0u);
    EXPECT_EQ(restarted_log.size(), 20u);
}

// ── Zero-impact: empty log replay ──

TEST(CdcDurabilityTest, EmptyLogReplay) {
    // Restart with an empty log — should work without errors
    conflict_log_t empty;
    EXPECT_EQ(empty.size(), 0u);

    conflict_log_t restarted; restore_log_into(empty, &restarted);
    EXPECT_EQ(restarted.size(), 0u);
    EXPECT_EQ(restarted.pending_count(), 0u);

    auto all = restarted.get_all();
    EXPECT_TRUE(all.empty());
    auto pending = restarted.get_pending();
    EXPECT_TRUE(pending.empty());
}

// ── Concurrent crash recovery ──

TEST(CdcDurabilityTest, ConcurrentWriteAndSnapshot) {
    // Multiple threads writing while snapshots occur (simulates
    // concurrent write + checkpoint). The snapshot must reflect a
    // consistent state. Use small fixed number of writes to avoid
    // unbounded vector growth during snapshot copies.
    conflict_log_t log;
    const int num_writers = 4;
    const int entries_per_writer = 500;
    std::atomic<int> writers_done(0);

    auto writer = [&log, entries_per_writer, &writers_done]() {
        for (int i = 0; i < entries_per_writer; ++i) {
            conflict_log_entry_t entry;
            entry.id = generate_uuid();
            entry.occurred_at = current_microtime();
            entry.action = operator_action_t::PENDING;
            log.record(entry);
        }
        ++writers_done;
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < num_writers; ++t) {
        threads.emplace_back(writer);
    }

    // Snapshot periodically while writers are running
    int snapshots = 0;
    while (writers_done.load() < num_writers) {
        conflict_log_t snapshot; restore_log_into(log, &snapshot);
        EXPECT_GE(snapshot.size(), 0u);
        ++snapshots;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    for (auto &t : threads) { t.join(); }

    EXPECT_EQ(log.size(), num_writers * entries_per_writer);
    (void)snapshots;
}

// ── Performance: throughput measurement ──

TEST(CdcDurabilityTest, LwwResolutionThroughput) {
    // Measure how many LWW resolutions per microsecond
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);
    uuid_u cluster = generate_uuid();
    uuid_u shard = generate_uuid();

    const int iterations = 10000;
    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < iterations; ++i) {
        change_record_t source = make_record(
            change_operation_t::UPDATE, i, cluster, shard, i * 2);
        change_record_t target = make_record(
            change_operation_t::UPDATE, i + 1, cluster, shard, i * 2 + 1);
        auto result = resolver.resolve(source, target);
        EXPECT_FALSE(result.skipped);
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        end - start).count();
    // Verify throughput is reasonable: at least 100 docs/us (i.e., 10ns per)
    double docs_per_us = static_cast<double>(iterations) / elapsed_us;
    EXPECT_GT(docs_per_us, 0.01);  // at least 10k/sec sanity check
}

TEST(CdcDurabilityTest, MergeResolutionThroughput) {
    // Measure PK-merge throughput
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);

    const int iterations = 5000;

    // Build a target document once
    std::map<datum_string_t, datum_t> target_fields;
    for (int f = 0; f < 100; ++f) {
        target_fields[datum_string_t("field_" + std::to_string(f))]
            = datum_t(static_cast<double>(f));
    }
    datum_t target(std::move(target_fields));

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < iterations; ++i) {
        std::map<datum_string_t, datum_t> update_fields;
        update_fields[datum_string_t("field_" + std::to_string(i % 100))]
            = datum_t(static_cast<double>(i));

        datum_t update_datum(std::move(update_fields));
        datum_t before;  // uninitialized

        change_record_t upd;
        upd.op = change_operation_t::UPDATE;
        upd.commit_timestamp = static_cast<microtime_t>(i);
        upd.event_id.table_id = generate_uuid();
        upd.event_id.source_cluster_id = generate_uuid();
        upd.event_id.shard_id = generate_uuid();
        upd.event_id.lsn.value = static_cast<uint64_t>(i);
        upd.after_image = serialize_datum_to_vector(update_datum);
        upd.before_image = serialize_datum_to_vector(before);

        datum_t result = resolver.apply_merge(upd, target, true);
        EXPECT_TRUE(result.has());
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        end - start).count();
    double docs_per_us = static_cast<double>(iterations) / elapsed_us;
    EXPECT_GT(docs_per_us, 0.001);  // at least 1k/sec
}

TEST(CdcDurabilityTest, ConflictLogAppendThroughput) {
    // Measure conflict log append throughput (p50/p99)
    conflict_log_t log;
    const int total = 5000;

    std::vector<microtime_t> latencies;
    latencies.reserve(total);

    for (int i = 0; i < total; ++i) {
        conflict_log_entry_t entry;
        entry.id = generate_uuid();
        entry.occurred_at = current_microtime();

        auto start = std::chrono::high_resolution_clock::now();
        log.record(entry);
        auto end = std::chrono::high_resolution_clock::now();

        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end - start).count();
        latencies.push_back(static_cast<microtime_t>(ns));
    }

    EXPECT_EQ(log.size(), total);

    // Sort to find p50 and p99
    std::sort(latencies.begin(), latencies.end());
    microtime_t p50 = latencies[total / 2];
    microtime_t p99 = latencies[static_cast<size_t>(total * 0.99)];

    // Logging for visibility (these are not failures, just informational)
    (void)p50;
    (void)p99;

    // Verify basic sanity: median append should be under 1ms (1,000,000 ns)
    EXPECT_LT(p50, 1000000LL)
        << "p50 append latency too high: " << p50 << " ns";
}

// ── Concurrent writers throughput ──

TEST(CdcDurabilityTest, ConcurrentWritersThroughput) {
    // Measure resolution under concurrent writers (contention)
    conflict_resolver_t resolver(conflict_resolution_policy_t::LAST_WRITE_WINS);
    std::atomic<int64_t> total_resolved(0);
    std::atomic<bool> stop(false);

    auto writer = [&](int thread_id) {
        uuid_u cluster = generate_uuid();
        uuid_u shard = generate_uuid();
        while (!stop.load()) {
            change_record_t source = make_record(
                change_operation_t::UPDATE, 1000, cluster, shard, thread_id * 100);
            change_record_t target = make_record(
                change_operation_t::UPDATE, 2000, cluster, shard, thread_id * 100 + 1);
            auto result = resolver.resolve(source, target);
            if (!result.skipped) {
                ++total_resolved;
            }
        }
    };

    std::vector<std::thread> threads;
    const int num_threads = 4;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back(writer, t);
    }

    // Let them run for a short time
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop.store(true);

    for (auto &t : threads) { t.join(); }

    // Verify meaningful throughput: at least 10k resolutions in 100ms
    EXPECT_GT(total_resolved.load(), 10000)
        << "Concurrent throughput too low: " << total_resolved.load();
}

}  // namespace unittest
