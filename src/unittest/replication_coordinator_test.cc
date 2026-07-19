// Copyright 2026 RethinkDB, all rights reserved.
#include "unittest/gtest.hpp"
#include <map>
#include "clustering/replication_coordinator.hpp"
#include "containers/uuid.hpp"
#include "rdb_protocol/publication.hpp"

namespace unittest {
namespace {

ql::replication_consumer_identity_t make_consumer() {
    ql::replication_consumer_identity_t c;
    c.consumer_id = generate_uuid();
    c.credential_ref = "secret://cdc/test";
    c.supported_schemas.push_back("internal_rdb_v1");
    return c;
}
ql::publication_config_t make_publication(uint64_t max_lag = 1000) {
    ql::publication_config_t pub;
    pub.publication_id = generate_uuid();
    pub.table_id = generate_uuid();
    pub.max_slot_lag_bytes = max_lag;
    return pub;
}

}  // namespace

TEST(ReplicationCoordinatorTest, CreateBindPauseResumeDropLifecycle) {
    ql::logical_log_retention_t ret;
    ql::replication_coordinator_t coord(&ret);
    auto pub = make_publication();
    uuid_u sid = generate_uuid(), shid = generate_uuid();
    coord.create_slot(sid, pub.publication_id, make_consumer());
    coord.bind_slot(sid, pub, {{shid, {10}}});
    auto s = coord.get_slot_state(sid);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(ql::replication_slot_state_t::SNAPSHOTTING, s->state);
    coord.advance_slot(sid, {shid, {10}}, 0);
    EXPECT_EQ(ql::replication_slot_state_t::STREAMING,
              coord.get_slot_state(sid)->state);
    coord.pause_slot(sid);
    EXPECT_EQ(ql::replication_slot_state_t::PAUSED,
              coord.get_slot_state(sid)->state);
    EXPECT_EQ(10u, ret.retention_floor(pub.table_id, shid).value);
    coord.resume_slot(sid);
    EXPECT_EQ(ql::replication_slot_state_t::STREAMING,
              coord.get_slot_state(sid)->state);
    coord.drop_slot(sid);
    EXPECT_FALSE(coord.get_slot_state(sid).has_value());
    EXPECT_EQ(0u, ret.retention_floor(pub.table_id, shid).value);
}

TEST(ReplicationCoordinatorTest, FlushOnlyPositionNeverReleasesRetention) {
    ql::logical_log_retention_t ret;
    ql::replication_coordinator_t coord(&ret);
    auto pub = make_publication();
    uuid_u sid = generate_uuid(), shid = generate_uuid();
    coord.create_slot(sid, pub.publication_id, make_consumer());
    coord.bind_slot(sid, pub, {{shid, {5}}});
    EXPECT_TRUE(coord.note_flush_lsn(sid, {shid, {50}}, 0));
    EXPECT_EQ(5u, ret.retention_floor(pub.table_id, shid).value);
    EXPECT_EQ(5u, coord.get_slot_state(sid)
                  ->confirmed_lsn_by_shard.at(shid).value);
    EXPECT_EQ(50u, coord.get_slot_state(sid)
                   ->flush_lsn_by_shard.at(shid).value);
}

TEST(ReplicationCoordinatorTest, ConfirmedLsnMustMatchCurrentIncarnationAndFlush) {
    ql::logical_log_retention_t ret;
    ql::replication_coordinator_t coord(&ret);
    auto pub = make_publication();
    uuid_u sid = generate_uuid(), shid = generate_uuid();
    coord.create_slot(sid, pub.publication_id, make_consumer());
    coord.bind_slot(sid, pub, {{shid, {3}}});
    EXPECT_TRUE(coord.note_flush_lsn(sid, {shid, {4}}, 0));
    EXPECT_FALSE(coord.confirm_lsn(sid, {shid, {5}}, 0));
    EXPECT_FALSE(coord.confirm_lsn(sid, {shid, {4}}, 1));
    EXPECT_TRUE(coord.confirm_lsn(sid, {shid, {4}}, 0));
    EXPECT_FALSE(coord.confirm_lsn(sid, {shid, {3}}, 0));
    EXPECT_EQ(4u, ret.retention_floor(pub.table_id, shid).value);
}

TEST(ReplicationCoordinatorTest, RetentionFloorTracksSlowestSlot) {
    ql::logical_log_retention_t ret;
    ql::replication_coordinator_t coord(&ret);
    auto pub = make_publication();
    uuid_u shid = generate_uuid();
    uuid_u slow = generate_uuid(), fast = generate_uuid();
    coord.create_slot(slow, pub.publication_id, make_consumer());
    coord.bind_slot(slow, pub, {{shid, {10}}});
    coord.create_slot(fast, pub.publication_id, make_consumer());
    coord.bind_slot(fast, pub, {{shid, {20}}});
    EXPECT_EQ(10u, ret.retention_floor(pub.table_id, shid).value);
    EXPECT_TRUE(coord.note_flush_lsn(slow, {shid, {15}}, 0));
    EXPECT_TRUE(coord.confirm_lsn(slow, {shid, {15}}, 0));
    EXPECT_EQ(15u, ret.retention_floor(pub.table_id, shid).value);
}

TEST(ReplicationCoordinatorTest, BackpressureIsBoundedWithoutBlockingWrites) {
    ql::logical_log_retention_t ret;
    ql::replication_coordinator_t coord(&ret);
    auto pub = make_publication();
    uuid_u sid = generate_uuid();
    coord.create_slot(sid, pub.publication_id, make_consumer());
    coord.configure_backpressure(sid, 2, 100);
    EXPECT_TRUE(coord.on_batch_enqueued(sid, 40));
    EXPECT_FALSE(coord.on_batch_enqueued(sid, 60));
    EXPECT_FALSE(coord.on_batch_enqueued(sid, 1));
    auto bp = coord.get_backpressure(sid);
    EXPECT_EQ(2u, bp.in_flight_batches);
    EXPECT_EQ(100u, bp.buffered_bytes);
    EXPECT_TRUE(bp.source_paused);
    coord.on_batch_dequeued(sid, 40);
    bp = coord.get_backpressure(sid);
    EXPECT_EQ(1u, bp.in_flight_batches);
    EXPECT_EQ(60u, bp.buffered_bytes);
    EXPECT_FALSE(bp.source_paused);
}

TEST(ReplicationCoordinatorTest, LagWarnsAtEightyPercentAndPausesAtHardLimit) {
    ql::logical_log_retention_t ret;
    ql::replication_coordinator_t coord(&ret);
    auto pub = make_publication(100);
    uuid_u sid = generate_uuid(), shid = generate_uuid();
    coord.create_slot(sid, pub.publication_id, make_consumer());
    coord.bind_slot(sid, pub, {{shid, {0}}});
    coord.note_journal_high_water(pub.table_id, shid, {80}, 80, 8000);
    auto lag = coord.get_slot_lag(sid);
    ASSERT_TRUE(lag.has_value());
    EXPECT_TRUE(lag->warn_threshold_breached);
    EXPECT_FALSE(lag->hard_threshold_breached);
    coord.note_journal_high_water(pub.table_id, shid, {100}, 100, 10000);
    lag = coord.get_slot_lag(sid);
    ASSERT_TRUE(lag.has_value());
    EXPECT_TRUE(lag->hard_threshold_breached);
    EXPECT_EQ(ql::replication_slot_state_t::PAUSED,
              coord.get_slot_state(sid)->state);
}

TEST(ReplicationCoordinatorTest, RoutingHandoffPreservesConfirmedCursor) {
    ql::logical_log_retention_t ret;
    ql::replication_coordinator_t coord(&ret);
    auto pub = make_publication();
    uuid_u sid = generate_uuid(), shid = generate_uuid();
    coord.create_slot(sid, pub.publication_id, make_consumer());
    coord.bind_slot(sid, pub, {{shid, {7}}});
    EXPECT_TRUE(coord.note_flush_lsn(sid, {shid, {9}}, 0));
    EXPECT_TRUE(coord.confirm_lsn(sid, {shid, {9}}, 0));
    coord.on_shard_routing_change(pub.publication_id,
                                  {shid, 2, 100});
    auto s = coord.get_slot_state(sid);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(9u, s->confirmed_lsn_by_shard.at(shid).value);
    EXPECT_EQ(2u, s->shard_incarnation_by_shard.at(shid));
}

TEST(ReplicationCoordinatorTest, EvictionReleasesPinButKeepsAuditState) {
    ql::logical_log_retention_t ret;
    ql::replication_coordinator_t coord(&ret);
    auto pub = make_publication();
    uuid_u sid = generate_uuid(), shid = generate_uuid();
    coord.create_slot(sid, pub.publication_id, make_consumer());
    coord.bind_slot(sid, pub, {{shid, {11}}});
    coord.evict_slot(sid, "hard_lag_limit", "quota reached");
    auto s = coord.get_slot_state(sid);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(ql::replication_slot_state_t::EVICTED, s->state);
    EXPECT_EQ(11u, s->confirmed_lsn_by_shard.at(shid).value);
    EXPECT_EQ(0u, ret.retention_floor(pub.table_id, shid).value);
}

}  // namespace unittest
