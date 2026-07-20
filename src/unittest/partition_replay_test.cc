// Copyright 2026 RethinkDB, all rights reserved.
#include "btree/partition_ops.hpp"
#include "btree/partition_replay.hpp"
#include "btree/reql_specific.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "clustering/administration/tables/table_metadata.hpp"
#include "containers/name_string.hpp"
#include "containers/uuid.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/partition_config.hpp"
#include "unittest/gtest.hpp"

namespace unittest {

namespace {

partition_entry_t make_range_entry(const char *name) {
    partition_entry_t e;
    e.id = generate_uuid();
    e.name = name_string_t::guarantee_valid(name);
    e.storage_id = generate_uuid();
    e.state = partition_state_t::CREATING;
    return e;
}

/* Two-way range layout: [minval, 100) and [100, maxval). */
partition_config_t make_range_candidate(uint64_t epoch) {
    partition_config_t cfg;
    cfg.type = partition_type_t::RANGE;
    cfg.key_field = "ts";
    cfg.epoch = epoch;
    cfg.partitions.push_back(make_range_entry("p_lo"));
    cfg.partitions.push_back(make_range_entry("p_hi"));
    cfg.range_boundaries.push_back(ql::datum_t::minval());
    cfg.range_boundaries.push_back(ql::datum_t(100.0));
    cfg.range_boundaries.push_back(ql::datum_t::maxval());
    cfg.hash_modulus = 0;
    return cfg;
}

transition_modification_t make_mod(
        const std::string &pk,
        double value,
        uint64_t stamp) {
    transition_modification_t m;
    m.primary_key = store_key_t(pk);
    m.value = ql::datum_t(value);
    m.mutation_stamp = stamp;
    return m;
}

transition_modification_t make_delete_mod(
        const std::string &pk,
        uint64_t stamp) {
    transition_modification_t m;
    m.primary_key = store_key_t(pk);
    m.value = ql::datum_t::null();
    m.mutation_stamp = stamp;
    return m;
}

}  // namespace

/* ── State machine (PART-06 edges used by PART-07) ───────────────────────── */

TEST(PartitionReplayTest, StateTransitionHappyPath) {
    EXPECT_EQ("", validate_partition_state_transition(
        partition_state_t::CREATING, partition_state_t::CATCHING_UP));
    EXPECT_EQ("", validate_partition_state_transition(
        partition_state_t::CATCHING_UP, partition_state_t::ACTIVE));
    EXPECT_EQ("", validate_partition_state_transition(
        partition_state_t::ACTIVE, partition_state_t::DRAINING));
}

TEST(PartitionReplayTest, StateTransitionFailEdges) {
    EXPECT_EQ("", validate_partition_state_transition(
        partition_state_t::CREATING, partition_state_t::FAILED));
    EXPECT_EQ("", validate_partition_state_transition(
        partition_state_t::CATCHING_UP, partition_state_t::FAILED));
}

TEST(PartitionReplayTest, StateTransitionIllegal) {
    EXPECT_NE("", validate_partition_state_transition(
        partition_state_t::CREATING, partition_state_t::ACTIVE));
    EXPECT_NE("", validate_partition_state_transition(
        partition_state_t::ACTIVE, partition_state_t::CATCHING_UP));
    EXPECT_NE("", validate_partition_state_transition(
        partition_state_t::DRAINING, partition_state_t::ACTIVE));
    EXPECT_NE("", validate_partition_state_transition(
        partition_state_t::FAILED, partition_state_t::ACTIVE));
}

TEST(PartitionReplayTest, ApplyStateTransition) {
    partition_store_ref_t store;
    store.state = partition_state_t::CREATING;
    store.epoch = 1;
    apply_partition_state_transition(&store, partition_state_t::CATCHING_UP);
    EXPECT_EQ(partition_state_t::CATCHING_UP, store.state);
    apply_partition_state_transition(&store, partition_state_t::ACTIVE);
    EXPECT_EQ(partition_state_t::ACTIVE, store.state);
    apply_partition_state_transition(&store, partition_state_t::DRAINING);
    EXPECT_EQ(partition_state_t::DRAINING, store.state);
}

/* ── Idempotent replay (primary key + mutation stamp) ────────────────────── */

TEST(PartitionReplayTest, IdempotentReplaySkipsOlderStamp) {
    std::map<store_key_t, partition_replay_row_t> rows;

    transition_modification_t m1 = make_mod("k1", 10.0, 1);
    transition_modification_t m2 = make_mod("k1", 20.0, 2);
    transition_modification_t m1_again = make_mod("k1", 99.0, 1);

    EXPECT_TRUE(partition_replay_t::apply_modification_idempotent(&rows, m1));
    EXPECT_TRUE(partition_replay_t::apply_modification_idempotent(&rows, m2));
    /* Replay of stamp 1 after stamp 2 must be a no-op. */
    EXPECT_FALSE(
        partition_replay_t::apply_modification_idempotent(&rows, m1_again));

    ASSERT_EQ(1u, rows.size());
    auto it = rows.find(store_key_t("k1"));
    ASSERT_TRUE(it != rows.end());
    EXPECT_EQ(2u, it->second.mutation_stamp);
    EXPECT_EQ(20.0, it->second.value.as_num());
}

TEST(PartitionReplayTest, IdempotentReplayEqualStampSkipped) {
    std::map<store_key_t, partition_replay_row_t> rows;
    transition_modification_t m = make_mod("k", 1.0, 5);
    EXPECT_TRUE(partition_replay_t::apply_modification_idempotent(&rows, m));
    EXPECT_FALSE(partition_replay_t::apply_modification_idempotent(&rows, m));
    EXPECT_EQ(1.0, rows[store_key_t("k")].value.as_num());
}

TEST(PartitionReplayTest, IdempotentReplayDelete) {
    std::map<store_key_t, partition_replay_row_t> rows;
    EXPECT_TRUE(partition_replay_t::apply_modification_idempotent(
        &rows, make_mod("k", 1.0, 1)));
    EXPECT_TRUE(partition_replay_t::apply_modification_idempotent(
        &rows, make_delete_mod("k", 2)));
    ASSERT_EQ(1, rows.count(store_key_t("k")));
    EXPECT_EQ(ql::datum_t::R_NULL, rows[store_key_t("k")].value.get_type());
    /* Older write after delete is skipped. */
    EXPECT_FALSE(partition_replay_t::apply_modification_idempotent(
        &rows, make_mod("k", 50.0, 1)));
    EXPECT_EQ(ql::datum_t::R_NULL, rows[store_key_t("k")].value.get_type());
}

TEST(PartitionReplayTest, EnqueueAndReplayProducesIdempotentResults) {
    /* Simulate durable queue + double replay (crash restart). */
    partition_catalog_t catalog;
    catalog.format_version = PARTITION_CATALOG_FORMAT_VERSION;
    catalog.epoch = 3;
    catalog.transition_active = true;
    catalog.transition_source_epoch = 3;
    catalog.next_mutation_stamp = 1;
    catalog.high_water_mark = 0;

    auto enqueue = [&](const std::string &pk, double v) {
        transition_modification_t m;
        m.primary_key = store_key_t(pk);
        m.value = ql::datum_t(v);
        m.mutation_stamp = catalog.next_mutation_stamp++;
        catalog.transition_queue.push_back(std::move(m));
    };

    enqueue("a", 1.0);
    enqueue("b", 2.0);
    enqueue("a", 3.0);  /* later update to a */

    catalog.high_water_mark = catalog.next_mutation_stamp - 1;
    ASSERT_EQ(3u, catalog.high_water_mark);

    /* Target store in CATCHING_UP so targets_caught_up can succeed. */
    partition_store_ref_t tgt;
    tgt.partition_id = generate_uuid();
    tgt.storage_id = generate_uuid();
    tgt.state = partition_state_t::CATCHING_UP;
    tgt.epoch = 4;
    catalog.stores.push_back(tgt);

    std::map<store_key_t, partition_replay_row_t> rows;
    /* First replay. */
    for (const transition_modification_t &mod : catalog.transition_queue) {
        if (mod.mutation_stamp <= catalog.high_water_mark) {
            partition_replay_t::apply_modification_idempotent(&rows, mod);
        }
    }
    /* Second replay (crash recovery) — must be idempotent. */
    for (const transition_modification_t &mod : catalog.transition_queue) {
        if (mod.mutation_stamp <= catalog.high_water_mark) {
            partition_replay_t::apply_modification_idempotent(&rows, mod);
        }
    }

    EXPECT_EQ(3.0, rows[store_key_t("a")].value.as_num());
    EXPECT_EQ(3u, rows[store_key_t("a")].mutation_stamp);
    EXPECT_EQ(2.0, rows[store_key_t("b")].value.as_num());
    EXPECT_EQ(2u, rows.size());
}

/* ── Failed repartition: source stays authoritative ──────────────────────── */

TEST(PartitionReplayTest, AbortUnpublishedLeavesSourceAuthoritative) {
    partition_catalog_t catalog;
    catalog.format_version = PARTITION_CATALOG_FORMAT_VERSION;
    catalog.epoch = 7;
    catalog.transition_active = true;
    catalog.transition_source_epoch = 7;
    catalog.next_mutation_stamp = 5;
    catalog.high_water_mark = 4;

    /* Source ACTIVE store at epoch 7. */
    partition_store_ref_t source;
    source.partition_id = generate_uuid();
    source.storage_id = generate_uuid();
    source.state = partition_state_t::ACTIVE;
    source.epoch = 7;
    source.shard_superblocks.push_back(NULL_BLOCK_ID);
    catalog.stores.push_back(source);

    /* Unpublished target at epoch 8 still CATCHING_UP. */
    partition_store_ref_t target;
    target.partition_id = generate_uuid();
    target.storage_id = generate_uuid();
    target.state = partition_state_t::CATCHING_UP;
    target.epoch = 8;
    target.shard_superblocks.push_back(NULL_BLOCK_ID);
    catalog.stores.push_back(target);

    /* Queued mutations that must be discarded on abort. */
    catalog.transition_queue.push_back(make_mod("x", 1.0, 1));

    partition_replay_t::abort_unpublished_transition(&catalog);

    EXPECT_EQ(7u, catalog.epoch);
    EXPECT_FALSE(catalog.transition_active);
    EXPECT_TRUE(catalog.transition_queue.empty());
    EXPECT_EQ(0u, catalog.high_water_mark);

    /* Source remains; target FAILED was retired. */
    ASSERT_EQ(1u, catalog.stores.size());
    EXPECT_EQ(partition_state_t::ACTIVE, catalog.stores[0].state);
    EXPECT_EQ(7u, catalog.stores[0].epoch);
}

TEST(PartitionReplayTest, CommitCutoverAdvancesEpochAndDrainsSource) {
    partition_catalog_t catalog;
    catalog.format_version = PARTITION_CATALOG_FORMAT_VERSION;
    catalog.epoch = 2;
    catalog.transition_active = true;
    catalog.transition_source_epoch = 2;
    catalog.next_mutation_stamp = 10;
    catalog.high_water_mark = 9;

    partition_store_ref_t source;
    source.partition_id = generate_uuid();
    source.storage_id = generate_uuid();
    source.state = partition_state_t::ACTIVE;
    source.epoch = 2;
    catalog.stores.push_back(source);

    partition_store_ref_t target;
    target.partition_id = generate_uuid();
    target.storage_id = generate_uuid();
    target.state = partition_state_t::CATCHING_UP;
    target.epoch = 3;
    catalog.stores.push_back(target);

    ASSERT_TRUE(partition_replay_t::commit_catalog_cutover(&catalog, 2));

    EXPECT_EQ(3u, catalog.epoch);
    EXPECT_FALSE(catalog.transition_active);
    EXPECT_TRUE(catalog.transition_queue.empty());

    ASSERT_EQ(2u, catalog.stores.size());
    EXPECT_EQ(partition_state_t::DRAINING, catalog.stores[0].state);
    EXPECT_EQ(2u, catalog.stores[0].epoch);
    EXPECT_EQ(partition_state_t::ACTIVE, catalog.stores[1].state);
    EXPECT_EQ(3u, catalog.stores[1].epoch);
}

TEST(PartitionReplayTest, CommitCutoverRejectsWrongEpoch) {
    partition_catalog_t catalog;
    catalog.epoch = 2;
    catalog.transition_active = true;
    catalog.transition_source_epoch = 2;
    EXPECT_FALSE(partition_replay_t::commit_catalog_cutover(&catalog, 1));
    EXPECT_EQ(2u, catalog.epoch);
    EXPECT_TRUE(catalog.transition_active);
}

TEST(PartitionReplayTest, RaftSetPartitionConfigEpochCheck) {
    table_config_and_shards_t cas;
    cas.config.partitioning = make_range_candidate(5);
    cas.config.partitioning.epoch = 5;

    partition_config_t next = make_range_candidate(6);
    for (partition_entry_t &e : next.partitions) {
        e.state = partition_state_t::ACTIVE;
    }

    table_config_and_shards_change_t::set_partition_config_t good;
    good.expected_epoch = 5;
    good.new_config = next;

    table_config_and_shards_change_t change(std::move(good));
    EXPECT_TRUE(change.apply_change(&cas));
    EXPECT_EQ(6u, cas.config.partitioning.epoch);

    /* Stale expected_epoch must not mutate. */
    table_config_and_shards_change_t::set_partition_config_t stale;
    stale.expected_epoch = 5;  /* current is 6 */
    stale.new_config = make_range_candidate(7);
    table_config_and_shards_change_t stale_change(std::move(stale));
    EXPECT_FALSE(stale_change.apply_change(&cas));
    EXPECT_EQ(6u, cas.config.partitioning.epoch);
}

TEST(PartitionReplayTest, MakeCutoverChangeCollectsProvisionalStores) {
    partition_catalog_t catalog;
    catalog.epoch = 1;

    partition_store_ref_t src;
    src.partition_id = generate_uuid();
    src.storage_id = generate_uuid();
    src.state = partition_state_t::ACTIVE;
    src.epoch = 1;
    catalog.stores.push_back(src);

    partition_store_ref_t tgt;
    tgt.partition_id = generate_uuid();
    tgt.storage_id = generate_uuid();
    tgt.state = partition_state_t::CATCHING_UP;
    tgt.epoch = 2;
    catalog.stores.push_back(tgt);

    partition_config_t next = make_range_candidate(2);
    auto change = partition_replay_t::make_cutover_change(1, next, catalog);
    EXPECT_EQ(1u, change.expected_epoch);
    EXPECT_EQ(2u, change.new_config.epoch);
    ASSERT_EQ(1u, change.provisional_stores.size());
    EXPECT_EQ(2u, change.provisional_stores[0].epoch);
    EXPECT_EQ(partition_state_t::CATCHING_UP,
              change.provisional_stores[0].state);
}

TEST(PartitionReplayTest, CreatePartitionStoresThenCatchingUp) {
    partition_config_t candidate = make_range_candidate(1);
    candidate.validate_or_throw();

    table_config_t table_config;
    partition_catalog_t provisional;
    partition_ops_t::create_partition_stores(
        candidate, table_config, &provisional, nullptr);

    ASSERT_EQ(2u, provisional.stores.size());
    for (const partition_store_ref_t &s : provisional.stores) {
        EXPECT_EQ(partition_state_t::CREATING, s.state);
        EXPECT_EQ(1u, s.epoch);
    }

    /* Simulate step 4 state advance. */
    for (partition_store_ref_t &s : provisional.stores) {
        apply_partition_state_transition(&s, partition_state_t::CATCHING_UP);
    }
    for (const partition_store_ref_t &s : provisional.stores) {
        EXPECT_EQ(partition_state_t::CATCHING_UP, s.state);
    }
}

TEST(PartitionReplayTest, RetireDrainedAfterCutover) {
    partition_catalog_t catalog;
    catalog.epoch = 4;

    partition_store_ref_t draining;
    draining.partition_id = generate_uuid();
    draining.storage_id = generate_uuid();
    draining.state = partition_state_t::DRAINING;
    draining.epoch = 3;
    catalog.stores.push_back(draining);

    partition_store_ref_t live;
    live.partition_id = generate_uuid();
    live.storage_id = generate_uuid();
    live.state = partition_state_t::ACTIVE;
    live.epoch = 4;
    catalog.stores.push_back(live);

    partition_ops_t::retire_drained_stores(4, &catalog);
    ASSERT_EQ(1u, catalog.stores.size());
    EXPECT_EQ(partition_state_t::ACTIVE, catalog.stores[0].state);
    EXPECT_EQ(4u, catalog.stores[0].epoch);
}

}  // namespace unittest
