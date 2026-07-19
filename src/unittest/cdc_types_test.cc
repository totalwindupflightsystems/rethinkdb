// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "unittest/gtest.hpp"

#include "rdb_protocol/cdc_types.hpp"
#include "rdb_protocol/publication.hpp"
#include "rdb_protocol/subscription.hpp"
#include "rdb_protocol/cdc_sink.hpp"
#include "rdb_protocol/replication.hpp"

namespace unittest {

// --- LSN ordering ---

TEST(CdcTypesTest, LsnOrdering) {
    ql::log_sequence_number_t a{10};
    ql::log_sequence_number_t b{20};
    ql::log_sequence_number_t c{10};

    EXPECT_TRUE(a < b);
    EXPECT_FALSE(b < a);
    EXPECT_TRUE(a == c);
    EXPECT_FALSE(a != c);
    EXPECT_TRUE(a != b);
    EXPECT_FALSE(a < a);
}

// --- change_event_id_t comparison ---

TEST(CdcTypesTest, ChangeEventIdComparison) {
    uuid_u cluster_a = generate_uuid();
    uuid_u cluster_b = generate_uuid();
    uuid_u table = generate_uuid();
    uuid_u shard_a = generate_uuid();
    uuid_u shard_b = generate_uuid();

    ql::change_event_id_t id1{cluster_a, table, shard_a, {10}};
    ql::change_event_id_t id2{cluster_a, table, shard_a, {20}};
    ql::change_event_id_t id3{cluster_a, table, shard_b, {5}};

    // Same cluster/table/shard + higher LSN = greater
    EXPECT_TRUE(id1 < id2);
    EXPECT_FALSE(id2 < id1);

    // Different shard: ordered by shard_id (exact order depends on UUID values)
    EXPECT_TRUE(id1 < id3 || id3 < id1);

    // Equality
    ql::change_event_id_t id1_copy{cluster_a, table, shard_a, {10}};
    EXPECT_TRUE(id1 == id1_copy);
    EXPECT_FALSE(id1 != id1_copy);
    EXPECT_TRUE(id1 != id2);

    // Different cluster
    ql::change_event_id_t id4{cluster_b, table, shard_a, {10}};
    EXPECT_TRUE(id1 != id4);
}

TEST(CdcTypesTest, ChangeEventIdSetOrdering) {
    uuid_u cluster = generate_uuid();
    uuid_u table = generate_uuid();
    uuid_u shard = generate_uuid();

    ql::change_event_id_t id1{cluster, table, shard, {10}};
    ql::change_event_id_t id2{cluster, table, shard, {20}};
    ql::change_event_id_t id3{cluster, table, shard, {30}};

    std::set<ql::change_event_id_t, ql::change_event_id_compare_by_lsn_t> s;
    s.insert(id2);
    s.insert(id1);
    s.insert(id3);

    // Should be ordered by LSN
    auto it = s.begin();
    EXPECT_EQ(it->lsn.value, 10U); ++it;
    EXPECT_EQ(it->lsn.value, 20U); ++it;
    EXPECT_EQ(it->lsn.value, 30U);
}

// --- change_operation_t enum ---

TEST(CdcTypesTest, ChangeOperationEnum) {
    EXPECT_EQ(static_cast<int>(ql::change_operation_t::INSERT), 0);
    EXPECT_EQ(static_cast<int>(ql::change_operation_t::UPDATE), 1);
    EXPECT_EQ(static_cast<int>(ql::change_operation_t::DELETE), 2);
    EXPECT_EQ(static_cast<int>(ql::change_operation_t::REPLACE), 3);
}

// --- publication_config_t defaults ---

TEST(PublicationTest, PublicationConfigDefaults) {
    ql::publication_config_t cfg;

    EXPECT_EQ(cfg.format, ql::publication_format_t::JSON_V1);
    EXPECT_TRUE(cfg.include_before_image);
    EXPECT_TRUE(cfg.include_after_image);
    EXPECT_EQ(cfg.default_snapshot_mode, ql::snapshot_mode_t::FULL);
    EXPECT_EQ(cfg.max_slot_lag_bytes, 1024ULL * 1024 * 1024);
    EXPECT_EQ(cfg.state, ql::publication_state_t::CREATING);
}

// --- publication_state_t values ---

TEST(PublicationTest, PublicationStateEnum) {
    EXPECT_EQ(static_cast<int>(ql::publication_state_t::CREATING), 0);
    EXPECT_EQ(static_cast<int>(ql::publication_state_t::READY), 1);
    EXPECT_EQ(static_cast<int>(ql::publication_state_t::DROPPING), 2);
    EXPECT_EQ(static_cast<int>(ql::publication_state_t::DROPPED), 3);
    EXPECT_EQ(static_cast<int>(ql::publication_state_t::ERROR), 4);
}

// --- subscription_config_t defaults ---

TEST(SubscriptionTest, SubscriptionConfigDefaults) {
    ql::subscription_config_t cfg;

    EXPECT_EQ(cfg.conflict_policy, ql::conflict_resolution_t::LAST_WRITE_WINS);
    EXPECT_EQ(cfg.state, ql::subscription_state_t::CREATING);
}

// --- subscription state machine transitions ---

TEST(SubscriptionTest, SubscriptionStateEnum) {
    EXPECT_EQ(static_cast<int>(ql::subscription_state_t::CREATING), 0);
    EXPECT_EQ(static_cast<int>(ql::subscription_state_t::CONNECTING), 1);
    EXPECT_EQ(static_cast<int>(ql::subscription_state_t::SNAPSHOTTING), 2);
    EXPECT_EQ(static_cast<int>(ql::subscription_state_t::CATCHING_UP), 3);
    EXPECT_EQ(static_cast<int>(ql::subscription_state_t::STREAMING), 4);
    EXPECT_EQ(static_cast<int>(ql::subscription_state_t::DROPPING), 5);
    EXPECT_EQ(static_cast<int>(ql::subscription_state_t::DROPPED), 6);
    EXPECT_EQ(static_cast<int>(ql::subscription_state_t::ERROR), 7);
}

// --- cdc_sink_config_t ---

TEST(CdcSinkTest, CdcSinkConfigDefaults) {
    ql::cdc_sink_config_t cfg;

    EXPECT_EQ(cfg.sink_type, ql::cdc_sink_type_t::KAFKA);
    EXPECT_EQ(cfg.state, ql::cdc_sink_state_t::CREATING);
}

TEST(CdcSinkTest, CdcBatchingConfigDefaults) {
    ql::cdc_batching_config_t cfg;

    EXPECT_EQ(cfg.max_records, 1000U);
    EXPECT_EQ(cfg.max_in_flight_batches, 5U);
    EXPECT_EQ(cfg.flush_interval_ms, 250U);
    EXPECT_EQ(cfg.max_buffer_bytes, 16ULL * 1024 * 1024);
}

// --- replication_slot_t lifecycle ---

TEST(ReplicationTest, ReplicationSlotDefaults) {
    ql::replication_slot_t slot;

    // Verify it's default-constructible
    EXPECT_TRUE(slot.name.empty());
}

TEST(ReplicationTest, ReplicationSlotKindEnum) {
    EXPECT_EQ(static_cast<int>(ql::replication_slot_kind_t::SUBSCRIPTION), 0);
    EXPECT_EQ(static_cast<int>(ql::replication_slot_kind_t::CDC_SINK), 1);
}

TEST(ReplicationTest, ReplicationSlotStateEnum) {
    EXPECT_EQ(static_cast<int>(ql::replication_slot_state_t::ACTIVE), 0);
    EXPECT_EQ(static_cast<int>(ql::replication_slot_state_t::PAUSED), 1);
    EXPECT_EQ(static_cast<int>(ql::replication_slot_state_t::ERROR), 2);
    EXPECT_EQ(static_cast<int>(ql::replication_slot_state_t::EVICTED), 3);
}

// --- subscription_applier_t dedup (CDC-06e API) ---

TEST(ReplicationTest, ApplierApplyBatchAndDedup) {
    ql::subscription_handle_t handle;
    ql::subscription_applier_t applier(&handle);
    cond_t never_interrupted;
    uuid_u cluster = generate_uuid();
    uuid_u table = generate_uuid();
    uuid_u shard = generate_uuid();

    // change_record_t needs non-empty after_image for INSERT
    ql::change_record_t r1;
    r1.event_id = ql::change_event_id_t{cluster, table, shard, {10}};
    r1.op = ql::change_operation_t::INSERT;
    r1.after_image = {'x'};
    r1.commit_timestamp = 1000.0;

    ql::change_record_t r2;
    r2.event_id = ql::change_event_id_t{cluster, table, shard, {20}};
    r2.op = ql::change_operation_t::INSERT;
    r2.after_image = {'y'};
    r2.commit_timestamp = 2000.0;

    // Neither should be in the ledger yet
    EXPECT_FALSE(applier.already_applied_no_interruptor(r1.event_id));
    EXPECT_FALSE(applier.already_applied_no_interruptor(r2.event_id));
    EXPECT_EQ(0U, applier.ledger_size());

    // Apply batch: both records should be recorded
    applier.apply_batch({r1, r2}, &never_interrupted);
    EXPECT_TRUE(applier.already_applied_no_interruptor(r1.event_id));
    EXPECT_TRUE(applier.already_applied_no_interruptor(r2.event_id));
    EXPECT_EQ(2U, applier.ledger_size());

    // Apply again: dedup should skip both (set semantics)
    applier.apply_batch({r1, r2}, &never_interrupted);
    EXPECT_TRUE(applier.already_applied_no_interruptor(r1.event_id));
    EXPECT_TRUE(applier.already_applied_no_interruptor(r2.event_id));
    EXPECT_EQ(2U, applier.ledger_size());  // unchanged
}

TEST(ReplicationTest, ApplierLedgerShardTracking) {
    ql::subscription_handle_t handle;
    ql::subscription_applier_t applier(&handle);
    cond_t never_interrupted;
    uuid_u cluster = generate_uuid();
    uuid_u table = generate_uuid();
    uuid_u shard_a = generate_uuid();
    uuid_u shard_b = generate_uuid();

    ql::change_record_t r_a1;
    r_a1.event_id = ql::change_event_id_t{cluster, table, shard_a, {10}};
    r_a1.op = ql::change_operation_t::INSERT;
    r_a1.after_image = {'a'};
    r_a1.commit_timestamp = 1000.0;

    ql::change_record_t r_a2;
    r_a2.event_id = ql::change_event_id_t{cluster, table, shard_a, {20}};
    r_a2.op = ql::change_operation_t::INSERT;
    r_a2.after_image = {'b'};
    r_a2.commit_timestamp = 2000.0;

    ql::change_record_t r_a3;
    r_a3.event_id = ql::change_event_id_t{cluster, table, shard_a, {30}};
    r_a3.op = ql::change_operation_t::INSERT;
    r_a3.after_image = {'c'};
    r_a3.commit_timestamp = 3000.0;

    ql::change_record_t r_b1;
    r_b1.event_id = ql::change_event_id_t{cluster, table, shard_b, {5}};
    r_b1.op = ql::change_operation_t::INSERT;
    r_b1.after_image = {'d'};
    r_b1.commit_timestamp = 500.0;

    // Apply all records
    applier.apply_batch({r_a1, r_a2, r_a3, r_b1}, &never_interrupted);
    EXPECT_EQ(4U, applier.ledger_size());
    EXPECT_TRUE(applier.ledger_has_shard(shard_a));
    EXPECT_TRUE(applier.ledger_has_shard(shard_b));

    // Confirmed LSN should have advanced per shard
    EXPECT_EQ(30U, handle.confirmed_lsn_by_shard[shard_a].value);
    EXPECT_EQ(5U, handle.confirmed_lsn_by_shard[shard_b].value);
}

} // namespace unittest
