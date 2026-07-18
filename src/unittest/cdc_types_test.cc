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

// --- subscription_applier_t dedup ---

TEST(ReplicationTest, ApplierRecordAndDedup) {
    ql::subscription_applier_t applier;
    uuid_u cluster = generate_uuid();
    uuid_u table = generate_uuid();
    uuid_u shard = generate_uuid();

    ql::change_event_id_t id1{cluster, table, shard, {10}};
    ql::change_event_id_t id2{cluster, table, shard, {20}};

    EXPECT_FALSE(applier.already_applied(id1));
    EXPECT_FALSE(applier.already_applied(id2));

    applier.record_apply(id1);
    EXPECT_TRUE(applier.already_applied(id1));
    EXPECT_FALSE(applier.already_applied(id2));

    // Dedup: recording again is a no-op (set semantics)
    applier.record_apply(id1);
    EXPECT_TRUE(applier.already_applied(id1));

    applier.record_apply(id2);
    EXPECT_TRUE(applier.already_applied(id2));
}

TEST(ReplicationTest, ApplierPruneBefore) {
    ql::subscription_applier_t applier;
    uuid_u cluster = generate_uuid();
    uuid_u table = generate_uuid();
    uuid_u shard_a = generate_uuid();
    uuid_u shard_b = generate_uuid();

    ql::change_event_id_t id1{cluster, table, shard_a, {10}};
    ql::change_event_id_t id2{cluster, table, shard_a, {20}};
    ql::change_event_id_t id3{cluster, table, shard_a, {30}};
    ql::change_event_id_t id4{cluster, table, shard_b, {5}};

    applier.record_apply(id1);
    applier.record_apply(id2);
    applier.record_apply(id3);
    applier.record_apply(id4);

    // Prune shard_a entries with LSN < 25
    ql::shard_lsn_t horizon{shard_a, {25}};
    applier.prune_before(horizon);

    EXPECT_FALSE(applier.already_applied(id1)); // LSN 10 < 25, pruned
    EXPECT_FALSE(applier.already_applied(id2)); // LSN 20 < 25, pruned
    EXPECT_TRUE(applier.already_applied(id3));  // LSN 30 >= 25, kept
    EXPECT_TRUE(applier.already_applied(id4));  // Different shard, kept
}

} // namespace unittest
