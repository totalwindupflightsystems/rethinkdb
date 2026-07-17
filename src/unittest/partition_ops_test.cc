// Copyright 2026 RethinkDB, all rights reserved.
/* PART-10 / architecture §8.1 items 6–8:
 * key_range_t decomposition safety, pk_directory_t, and superblock catalog
 * persistence via partition_ops_t. */
#include "btree/partition_ops.hpp"
#include "btree/pk_directory.hpp"
#include "btree/reql_specific.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "arch/io/disk.hpp"
#include "btree/operations.hpp"
#include "buffer_cache/cache_balancer.hpp"
#include "clustering/administration/tables/table_metadata.hpp"
#include "concurrency/cond_var.hpp"
#include "containers/binary_blob.hpp"
#include "containers/name_string.hpp"
#include "containers/uuid.hpp"
#include "protocol_api.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/partition_config.hpp"
#include "rpc/connectivity/server_id.hpp"
#include "serializer/log/log_serializer.hpp"
#include "unittest/gtest.hpp"
#include "unittest/unittest_utils.hpp"

namespace unittest {

namespace {

partition_entry_t make_entry(const char *name) {
    partition_entry_t e;
    e.id = generate_uuid();
    e.name = name_string_t::guarantee_valid(name);
    e.storage_id = generate_uuid();
    e.state = partition_state_t::ACTIVE;
    /* primary_key_range is a B-tree PK concept — default universe until the
     * storage layer assigns the partition's local primary-key slice. It must
     * never be derived from partition-field datums. */
    e.primary_key_range = key_range_t::universe();
    return e;
}

partition_config_t make_range_config() {
    partition_config_t cfg;
    cfg.type = partition_type_t::RANGE;
    cfg.key_field = "ts";
    cfg.epoch = 1;
    cfg.partitions.push_back(make_entry("p_lo"));
    cfg.partitions.push_back(make_entry("p_hi"));
    cfg.range_boundaries.push_back(ql::datum_t::minval());
    cfg.range_boundaries.push_back(ql::datum_t(100.0));
    cfg.range_boundaries.push_back(ql::datum_t::maxval());
    return cfg;
}

/* Manual construction matching btree_metainfo / btree_sindex patterns. */
void with_superblock(const std::function<void(cache_conn_t *)> &body) {
    temp_file_t temp_file;
    io_backender_t io_backender(file_direct_io_mode_t::buffered_desired);
    filepath_file_opener_t file_opener(temp_file.name(), &io_backender);
    log_serializer_t::create(
        &file_opener,
        log_serializer_t::static_config_t());
    log_serializer_t serializer(
        log_serializer_t::dynamic_config_t(),
        &file_opener,
        &get_global_perfmon_collection());
    dummy_cache_balancer_t balancer(GIGABYTE);
    cache_t cache(&serializer, &balancer, &get_global_perfmon_collection());
    cache_conn_t cache_conn(&cache);

    {
        txn_t txn(&cache_conn, write_durability_t::HARD, 1);
        {
            buf_lock_t sb_lock(&txn, SUPERBLOCK_ID, alt_create_t::create);
            real_superblock_t superblock(std::move(sb_lock));
            btree_slice_t::init_real_superblock(
                &superblock, std::vector<char>(), binary_blob_t());
        }
        txn.commit();
    }

    body(&cache_conn);
}

partition_catalog_t make_catalog_with_states() {
    partition_catalog_t cat;
    cat.format_version = PARTITION_CATALOG_FORMAT_VERSION;
    cat.epoch = 3;
    cat.primary_key_directory_block = NULL_BLOCK_ID;

    partition_store_ref_t active;
    active.partition_id = generate_uuid();
    active.storage_id = generate_uuid();
    active.shard_superblocks = {NULL_BLOCK_ID};
    active.state = partition_state_t::ACTIVE;
    active.epoch = 3;

    partition_store_ref_t failed;
    failed.partition_id = generate_uuid();
    failed.storage_id = generate_uuid();
    failed.shard_superblocks = {NULL_BLOCK_ID};
    failed.state = partition_state_t::FAILED;
    failed.epoch = 2;

    partition_store_ref_t drained;
    drained.partition_id = generate_uuid();
    drained.storage_id = generate_uuid();
    drained.shard_superblocks = {NULL_BLOCK_ID};
    drained.state = partition_state_t::DRAINING;
    drained.epoch = 1;

    cat.stores.push_back(active);
    cat.stores.push_back(failed);
    cat.stores.push_back(drained);
    return cat;
}

}  // namespace

/* ── 6. key_range_t decomposition safety ─────────────────────────────────── */

TEST(PartitionOpsTest, PartitionFieldValuesNeverBecomeBtreePrimaryKeys) {
    /* Safety property (spec §4.2 / §8.1.6): partition-key datums live in the
     * partition-key domain (ql::datum_t range_boundaries / list_values). The
     * B-tree primary-key domain is key_range_t / store_key_t. Routing must not
     * construct store_key_t from partition-field values. */
    partition_config_t cfg = make_range_config();
    cfg.validate_or_throw();

    /* Partition keys are numeric timestamps in the datum domain. */
    const ql::datum_t partition_key_lo(50.0);
    const ql::datum_t partition_key_hi(150.0);
    const partition_entry_t *p_lo = cfg.route(partition_key_lo);
    const partition_entry_t *p_hi = cfg.route(partition_key_hi);
    ASSERT_NE(nullptr, p_lo);
    ASSERT_NE(nullptr, p_hi);
    EXPECT_NE(p_lo->id, p_hi->id);

    /* primary_key_range is a B-tree concept. Entries ship with the local PK
     * universe (or empty), independent of the partition-field datum that
     * selected them. Encoding the partition key as a store_key_t and claiming
     * it is the primary-key range would violate the safety property. */
    for (const partition_entry_t &e : cfg.partitions) {
        EXPECT_EQ(key_range_t::universe(), e.primary_key_range);

        store_key_t bogus_from_partition_field("50");
        key_range_t bogus_one_key =
            key_range_t::one_key(bogus_from_partition_field);
        EXPECT_FALSE(e.primary_key_range == bogus_one_key);
    }

    /* partition_map_t carries primary_ranges as key_range_t (B-tree), while
     * routing decisions use partition_config_t::route on datums. */
    partition_map_t map;
    map.epoch = cfg.epoch;
    for (const partition_entry_t &e : cfg.partitions) {
        map.stores[e.id] = e.storage_id;
        map.primary_ranges[e.id] = key_range_t::universe();
    }
    partition_selection_t sel;
    sel.partition_ids.push_back(p_lo->id);
    std::vector<partition_route_t> routes = map.routes_for(sel);
    ASSERT_EQ(1u, routes.size());
    EXPECT_EQ(p_lo->id, routes[0].partition_id);
    EXPECT_EQ(p_lo->storage_id, routes[0].storage_id);
    /* The route carries no partition-field value — only partition/storage IDs
     * and epoch. Shard decomposition will use primary_ranges / key_range_t. */
    EXPECT_EQ(cfg.epoch, routes[0].epoch);
    EXPECT_EQ(key_range_t::universe(), map.primary_ranges[p_lo->id]);
}

TEST(PartitionOpsTest, RangeBoundariesAreDatumsNotStoreKeys) {
    partition_config_t cfg = make_range_config();
    for (const ql::datum_t &b : cfg.range_boundaries) {
        ASSERT_TRUE(b.has());
        /* Boundaries are ReQL datums (minval / number / maxval), not btree
         * keys. */
        EXPECT_TRUE(
            b.get_type() == ql::datum_t::MINVAL
            || b.get_type() == ql::datum_t::MAXVAL
            || b.get_type() == ql::datum_t::R_NUM);
    }
}

/* ── 7. PK directory ─────────────────────────────────────────────────────── */

TPTEST(PartitionOpsTest, PkDirectoryInsertLookupExistsRemove) {
    with_superblock([](cache_conn_t *cache_conn) {
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        get_btree_superblock_and_txn_for_writing(
            cache_conn, nullptr, write_access_t::write, 1,
            write_durability_t::SOFT, &superblock, &txn);

        block_id_t dir_id = NULL_BLOCK_ID;
        uuid_u part_a = generate_uuid();
        store_key_t pk("user:1");

        EXPECT_TRUE(pk_directory_t::try_insert(
            txn.get(), superblock->expose_buf(), &dir_id, pk, part_a));
        EXPECT_NE(NULL_BLOCK_ID, dir_id);

        EXPECT_TRUE(pk_directory_t::exists(
            txn.get(), superblock->expose_buf(), dir_id, pk));
        EXPECT_EQ(part_a,
                  pk_directory_t::lookup(
                      txn.get(), superblock->expose_buf(), dir_id, pk));

        pk_directory_t::remove(
            txn.get(), superblock->expose_buf(), &dir_id, pk);
        EXPECT_FALSE(pk_directory_t::exists(
            txn.get(), superblock->expose_buf(), dir_id, pk));
        EXPECT_TRUE(
            pk_directory_t::lookup(
                txn.get(), superblock->expose_buf(), dir_id, pk).is_nil());

        /* Re-insert after remove must succeed. */
        uuid_u part_b = generate_uuid();
        EXPECT_TRUE(pk_directory_t::try_insert(
            txn.get(), superblock->expose_buf(), &dir_id, pk, part_b));
        EXPECT_EQ(part_b,
                  pk_directory_t::lookup(
                      txn.get(), superblock->expose_buf(), dir_id, pk));

        txn->commit();
    });
}

TPTEST(PartitionOpsTest, PkDirectoryDuplicateDetection) {
    with_superblock([](cache_conn_t *cache_conn) {
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        get_btree_superblock_and_txn_for_writing(
            cache_conn, nullptr, write_access_t::write, 1,
            write_durability_t::SOFT, &superblock, &txn);

        block_id_t dir_id = NULL_BLOCK_ID;
        uuid_u part_a = generate_uuid();
        uuid_u part_b = generate_uuid();
        store_key_t pk("dup");

        EXPECT_TRUE(pk_directory_t::try_insert(
            txn.get(), superblock->expose_buf(), &dir_id, pk, part_a));
        EXPECT_FALSE(pk_directory_t::try_insert(
            txn.get(), superblock->expose_buf(), &dir_id, pk, part_b));
        /* Original owner preserved. */
        EXPECT_EQ(part_a,
                  pk_directory_t::lookup(
                      txn.get(), superblock->expose_buf(), dir_id, pk));

        txn->commit();
    });
}

TPTEST(PartitionOpsTest, PkDirectoryMoveEntryAtomic) {
    with_superblock([](cache_conn_t *cache_conn) {
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        get_btree_superblock_and_txn_for_writing(
            cache_conn, nullptr, write_access_t::write, 1,
            write_durability_t::SOFT, &superblock, &txn);

        block_id_t dir_id = NULL_BLOCK_ID;
        uuid_u src = generate_uuid();
        uuid_u dst = generate_uuid();
        store_key_t pk("move-me");

        ASSERT_TRUE(pk_directory_t::try_insert(
            txn.get(), superblock->expose_buf(), &dir_id, pk, src));

        pk_directory_t::move_entry(
            txn.get(), superblock->expose_buf(), &dir_id, pk, src, dst);

        EXPECT_EQ(dst,
                  pk_directory_t::lookup(
                      txn.get(), superblock->expose_buf(), dir_id, pk));
        EXPECT_TRUE(pk_directory_t::exists(
            txn.get(), superblock->expose_buf(), dir_id, pk));

        /* Wrong source must throw and leave ownership unchanged. */
        uuid_u other = generate_uuid();
        EXPECT_THROW(
            pk_directory_t::move_entry(
                txn.get(), superblock->expose_buf(), &dir_id, pk, src, other),
            cannot_perform_query_exc_t);
        EXPECT_EQ(dst,
                  pk_directory_t::lookup(
                      txn.get(), superblock->expose_buf(), dir_id, pk));

        txn->commit();
    });
}

TPTEST(PartitionOpsTest, PkDirectoryRemoveAllForPartition) {
    with_superblock([](cache_conn_t *cache_conn) {
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        get_btree_superblock_and_txn_for_writing(
            cache_conn, nullptr, write_access_t::write, 1,
            write_durability_t::SOFT, &superblock, &txn);

        block_id_t dir_id = NULL_BLOCK_ID;
        uuid_u part_a = generate_uuid();
        uuid_u part_b = generate_uuid();

        ASSERT_TRUE(pk_directory_t::try_insert(
            txn.get(), superblock->expose_buf(), &dir_id,
            store_key_t("a1"), part_a));
        ASSERT_TRUE(pk_directory_t::try_insert(
            txn.get(), superblock->expose_buf(), &dir_id,
            store_key_t("a2"), part_a));
        ASSERT_TRUE(pk_directory_t::try_insert(
            txn.get(), superblock->expose_buf(), &dir_id,
            store_key_t("b1"), part_b));

        pk_directory_t::remove_all_for_partition(
            txn.get(), superblock->expose_buf(), &dir_id, part_a);

        EXPECT_FALSE(pk_directory_t::exists(
            txn.get(), superblock->expose_buf(), dir_id, store_key_t("a1")));
        EXPECT_FALSE(pk_directory_t::exists(
            txn.get(), superblock->expose_buf(), dir_id, store_key_t("a2")));
        EXPECT_TRUE(pk_directory_t::exists(
            txn.get(), superblock->expose_buf(), dir_id, store_key_t("b1")));
        EXPECT_EQ(part_b,
                  pk_directory_t::lookup(
                      txn.get(), superblock->expose_buf(), dir_id,
                      store_key_t("b1")));

        txn->commit();
    });
}

TPTEST(PartitionOpsTest, PkDirectoryReleaseCleanup) {
    with_superblock([](cache_conn_t *cache_conn) {
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        get_btree_superblock_and_txn_for_writing(
            cache_conn, nullptr, write_access_t::write, 1,
            write_durability_t::SOFT, &superblock, &txn);

        block_id_t dir_id = NULL_BLOCK_ID;
        ASSERT_TRUE(pk_directory_t::try_insert(
            txn.get(), superblock->expose_buf(), &dir_id,
            store_key_t("x"), generate_uuid()));
        ASSERT_NE(NULL_BLOCK_ID, dir_id);

        pk_directory_t::release(
            txn.get(), superblock->expose_buf(), &dir_id);
        EXPECT_EQ(NULL_BLOCK_ID, dir_id);

        /* Release is idempotent. */
        pk_directory_t::release(
            txn.get(), superblock->expose_buf(), &dir_id);
        EXPECT_EQ(NULL_BLOCK_ID, dir_id);

        /* Lookup against absent directory yields nil. */
        EXPECT_TRUE(
            pk_directory_t::lookup(
                txn.get(), superblock->expose_buf(), NULL_BLOCK_ID,
                store_key_t("x")).is_nil());

        txn->commit();
    });
}

/* ── 8. Superblock allocation / release (catalog persistence) ───────────── */

TPTEST(PartitionOpsTest, CatalogLoadEmptyWhenAbsent) {
    with_superblock([](cache_conn_t *cache_conn) {
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        get_btree_superblock_and_txn_for_writing(
            cache_conn, nullptr, write_access_t::write, 1,
            write_durability_t::SOFT, &superblock, &txn);

        EXPECT_EQ(NULL_BLOCK_ID, superblock->get_partition_catalog_block_id());
        partition_catalog_t cat =
            partition_ops_t::load_catalog(txn.get(), superblock.get());
        EXPECT_EQ(0u, cat.format_version);
        EXPECT_EQ(0u, cat.epoch);
        EXPECT_TRUE(cat.stores.empty());
        EXPECT_EQ(NULL_BLOCK_ID, cat.primary_key_directory_block);

        txn->commit();
    });
}

TPTEST(PartitionOpsTest, CatalogSaveLoadRoundTripActiveFailedDrained) {
    with_superblock([](cache_conn_t *cache_conn) {
        partition_catalog_t original = make_catalog_with_states();
        ASSERT_EQ(3u, original.stores.size());
        EXPECT_EQ(partition_state_t::ACTIVE, original.stores[0].state);
        EXPECT_EQ(partition_state_t::FAILED, original.stores[1].state);
        EXPECT_EQ(partition_state_t::DRAINING, original.stores[2].state);

        uuid_u active_id = original.stores[0].partition_id;
        uuid_u failed_id = original.stores[1].partition_id;
        uuid_u drained_id = original.stores[2].partition_id;

        {
            scoped_ptr_t<txn_t> txn;
            scoped_ptr_t<real_superblock_t> superblock;
            get_btree_superblock_and_txn_for_writing(
                cache_conn, nullptr, write_access_t::write, 1,
                write_durability_t::SOFT, &superblock, &txn);
            partition_ops_t::save_catalog(
                txn.get(), superblock.get(), original);
            EXPECT_NE(NULL_BLOCK_ID,
                      superblock->get_partition_catalog_block_id());
            txn->commit();
        }

        {
            scoped_ptr_t<txn_t> txn;
            scoped_ptr_t<real_superblock_t> superblock;
            get_btree_superblock_and_txn_for_writing(
                cache_conn, nullptr, write_access_t::write, 1,
                write_durability_t::SOFT, &superblock, &txn);
            partition_catalog_t loaded =
                partition_ops_t::load_catalog(txn.get(), superblock.get());
            EXPECT_EQ(original.format_version, loaded.format_version);
            EXPECT_EQ(original.epoch, loaded.epoch);
            ASSERT_EQ(3u, loaded.stores.size());
            EXPECT_EQ(active_id, loaded.stores[0].partition_id);
            EXPECT_EQ(failed_id, loaded.stores[1].partition_id);
            EXPECT_EQ(drained_id, loaded.stores[2].partition_id);
            EXPECT_EQ(partition_state_t::ACTIVE, loaded.stores[0].state);
            EXPECT_EQ(partition_state_t::FAILED, loaded.stores[1].state);
            EXPECT_EQ(partition_state_t::DRAINING, loaded.stores[2].state);
            EXPECT_EQ(3u, loaded.stores[0].epoch);
            EXPECT_EQ(2u, loaded.stores[1].epoch);
            EXPECT_EQ(1u, loaded.stores[2].epoch);
            txn->commit();
        }
    });
}

TPTEST(PartitionOpsTest, CatalogReleaseClearsSuperblockRef) {
    with_superblock([](cache_conn_t *cache_conn) {
        {
            scoped_ptr_t<txn_t> txn;
            scoped_ptr_t<real_superblock_t> superblock;
            get_btree_superblock_and_txn_for_writing(
                cache_conn, nullptr, write_access_t::write, 1,
                write_durability_t::SOFT, &superblock, &txn);
            partition_ops_t::save_catalog(
                txn.get(), superblock.get(), make_catalog_with_states());
            ASSERT_NE(NULL_BLOCK_ID,
                      superblock->get_partition_catalog_block_id());
            txn->commit();
        }

        {
            scoped_ptr_t<txn_t> txn;
            scoped_ptr_t<real_superblock_t> superblock;
            get_btree_superblock_and_txn_for_writing(
                cache_conn, nullptr, write_access_t::write, 1,
                write_durability_t::SOFT, &superblock, &txn);
            partition_ops_t::release_catalog_block(
                txn.get(), superblock.get());
            EXPECT_EQ(NULL_BLOCK_ID,
                      superblock->get_partition_catalog_block_id());

            partition_catalog_t empty =
                partition_ops_t::load_catalog(txn.get(), superblock.get());
            EXPECT_TRUE(empty.stores.empty());
            EXPECT_EQ(0u, empty.epoch);

            /* Idempotent release. */
            partition_ops_t::release_catalog_block(
                txn.get(), superblock.get());
            EXPECT_EQ(NULL_BLOCK_ID,
                      superblock->get_partition_catalog_block_id());
            txn->commit();
        }
    });
}

TEST(PartitionOpsTest, CreatePartitionStoresProvisionalCatalog) {
    partition_config_t cfg = make_range_config();
    cfg.validate_or_throw();

    table_config_t table_config;
    table_config.basic.name = name_string_t::guarantee_valid("events");
    table_config.basic.primary_key = "id";
    table_config.write_ack_config = write_ack_config_t::SINGLE;
    table_config.durability = write_durability_t::HARD;
    /* One logical shard → one NULL superblock root slot per store. */
    table_config_t::shard_t shard;
    shard.primary_replica = server_id_t::generate_server_id();
    shard.all_replicas.insert(shard.primary_replica);
    table_config.shards.push_back(shard);

    partition_catalog_t catalog;
    cond_t non_interruptor;
    partition_ops_t::create_partition_stores(
        cfg, table_config, &catalog, &non_interruptor);

    EXPECT_EQ(PARTITION_CATALOG_FORMAT_VERSION, catalog.format_version);
    EXPECT_EQ(cfg.epoch, catalog.epoch);
    EXPECT_EQ(NULL_BLOCK_ID, catalog.primary_key_directory_block);
    ASSERT_EQ(cfg.partitions.size(), catalog.stores.size());
    for (size_t i = 0; i < catalog.stores.size(); ++i) {
        EXPECT_EQ(cfg.partitions[i].id, catalog.stores[i].partition_id);
        EXPECT_EQ(cfg.partitions[i].storage_id, catalog.stores[i].storage_id);
        EXPECT_EQ(partition_state_t::CREATING, catalog.stores[i].state);
        EXPECT_EQ(cfg.epoch, catalog.stores[i].epoch);
        ASSERT_EQ(1u, catalog.stores[i].shard_superblocks.size());
        EXPECT_EQ(NULL_BLOCK_ID, catalog.stores[i].shard_superblocks[0]);
    }
}

TPTEST(PartitionOpsTest, EnsurePkDirectoryPersistsInCatalog) {
    with_superblock([](cache_conn_t *cache_conn) {
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        get_btree_superblock_and_txn_for_writing(
            cache_conn, nullptr, write_access_t::write, 1,
            write_durability_t::SOFT, &superblock, &txn);

        partition_catalog_t catalog = make_catalog_with_states();
        partition_ops_t::save_catalog(
            txn.get(), superblock.get(), catalog);

        partition_ops_t::ensure_pk_directory(
            txn.get(), superblock.get(), &catalog);
        EXPECT_NE(NULL_BLOCK_ID, catalog.primary_key_directory_block);

        /* Second call is a no-op (same block id). */
        block_id_t first = catalog.primary_key_directory_block;
        partition_ops_t::ensure_pk_directory(
            txn.get(), superblock.get(), &catalog);
        EXPECT_EQ(first, catalog.primary_key_directory_block);

        partition_catalog_t reloaded =
            partition_ops_t::load_catalog(txn.get(), superblock.get());
        EXPECT_EQ(first, reloaded.primary_key_directory_block);

        txn->commit();
    });
}

TEST(PartitionOpsTest, RetireDrainedAndFailedStores) {
    partition_catalog_t catalog = make_catalog_with_states();
    /* minimum_live_epoch = 3 → DRAINING epoch 1 is retired; FAILED always
     * retired; ACTIVE epoch 3 stays. */
    partition_ops_t::retire_drained_stores(3, &catalog);
    ASSERT_EQ(1u, catalog.stores.size());
    EXPECT_EQ(partition_state_t::ACTIVE, catalog.stores[0].state);
    EXPECT_EQ(3u, catalog.stores[0].epoch);
}

}  // namespace unittest
