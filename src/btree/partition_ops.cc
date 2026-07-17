// Copyright 2026 RethinkDB, all rights reserved.
#include "btree/partition_ops.hpp"

#include <string.h>

#include <string>
#include <utility>
#include <vector>

#include "buffer_cache/alt.hpp"
#include "buffer_cache/blob.hpp"
#include "buffer_cache/serialize_onto_blob.hpp"
#include "clustering/administration/tables/table_metadata.hpp"
#include "concurrency/interruptor.hpp"
#include "containers/archive/buffer_group_stream.hpp"
#include "containers/archive/versioned.hpp"
#include "containers/uuid.hpp"
#include "errors.hpp"
#include "protocol_api.hpp"
#include "utils.hpp"

std::string validate_partition_state_transition(
        partition_state_t from, partition_state_t to) {
    /* Legal edges (section 5.3):
         CREATING    → CATCHING_UP | FAILED
         CATCHING_UP → ACTIVE      | FAILED
         ACTIVE      → DRAINING
       Retirement of DRAINING / FAILED is entry removal, not a state edge. */
    switch (from) {
    case partition_state_t::CREATING:
        if (to == partition_state_t::CATCHING_UP ||
            to == partition_state_t::FAILED) {
            return std::string();
        }
        break;
    case partition_state_t::CATCHING_UP:
        if (to == partition_state_t::ACTIVE ||
            to == partition_state_t::FAILED) {
            return std::string();
        }
        break;
    case partition_state_t::ACTIVE:
        if (to == partition_state_t::DRAINING) {
            return std::string();
        }
        break;
    case partition_state_t::DRAINING:
    case partition_state_t::FAILED:
        /* Terminal for state transitions; retirement is catalog removal. */
        break;
    default:
        unreachable();
    }

    return strprintf(
        "illegal partition state transition from %d to %d",
        static_cast<int>(from),
        static_cast<int>(to));
}

void apply_partition_state_transition(
        partition_store_ref_t *store, partition_state_t to) {
    guarantee(store != nullptr);
    std::string err = validate_partition_state_transition(store->state, to);
    if (!err.empty()) {
        throw cannot_perform_query_exc_t(err, query_state_t::FAILED);
    }
    store->state = to;
}

buf_lock_t partition_ops_t::allocate_catalog_block(real_superblock_t *sb) {
    /* Child block under the primary superblock. First btree_maxreflen bytes hold
    the blob ref (zeroed so blob_t starts from an empty ref). */
    buf_lock_t block(sb->expose_buf(), alt_create_t::create);
    {
        buf_write_t write(&block);
        char *ref_slot = static_cast<char *>(
            write.get_data_write(blob::btree_maxreflen));
        memset(ref_slot, 0, blob::btree_maxreflen);
    }
    return block;
}

partition_catalog_t partition_ops_t::load_catalog(
        txn_t *txn, real_superblock_t *sb) {
    guarantee(txn != nullptr);
    guarantee(sb != nullptr);

    partition_catalog_t catalog;
    block_id_t block_id = sb->get_partition_catalog_block_id();
    if (block_id == NULL_BLOCK_ID) {
        return catalog;
    }

    buf_lock_t catalog_block(sb->expose_buf(), block_id, access_t::read);

    /* Copy the blob ref out of the first btree_maxreflen bytes. blob_t may
    mutate the ref buffer in place, so we work from a mutable copy. */
    std::vector<char> ref_buf(blob::btree_maxreflen, 0);
    {
        buf_read_t ref_read(&catalog_block);
        uint32_t block_size;
        const char *ref_slot = static_cast<const char *>(
            ref_read.get_data_read(&block_size));
        guarantee(block_size >= static_cast<uint32_t>(blob::btree_maxreflen));
        memcpy(ref_buf.data(), ref_slot, blob::btree_maxreflen);
    }

    blob_t blob(sb->expose_buf().cache()->max_block_size(),
                ref_buf.data(),
                blob::btree_maxreflen);
    {
        buffer_group_t buffer_group;
        blob_acq_t acq;
        blob.expose_all(sb->expose_buf(), access_t::read, &buffer_group, &acq);
        /* Catalog format is new in Phase 3; always written as LATEST_DISK.
        Use the fixed-version path so we only need the SINCE_v2_4 instantiation. */
        deserialize_from_group<cluster_version_t::LATEST_DISK>(
            const_view(&buffer_group), &catalog);
    }
    return catalog;
}

void partition_ops_t::save_catalog(
        txn_t *txn, real_superblock_t *sb, const partition_catalog_t &catalog) {
    guarantee(txn != nullptr);
    guarantee(sb != nullptr);

    block_id_t block_id = sb->get_partition_catalog_block_id();
    buf_lock_t catalog_block;
    if (block_id == NULL_BLOCK_ID) {
        catalog_block = allocate_catalog_block(sb);
        block_id = catalog_block.block_id();
        sb->set_partition_catalog_block_id(block_id);
    } else {
        catalog_block = buf_lock_t(sb->expose_buf(), block_id, access_t::write);
    }

    /* Heap-allocated ref buffer. blob_t reads the ref at construction, then
    writes the updated ref after serialize_onto_blob (which clears + rewrites). */
    std::vector<char> ref_buf(blob::btree_maxreflen, 0);
    {
        buf_read_t ref_read(&catalog_block);
        uint32_t block_size;
        const char *ref_slot = static_cast<const char *>(
            ref_read.get_data_read(&block_size));
        guarantee(block_size >= static_cast<uint32_t>(blob::btree_maxreflen));
        memcpy(ref_buf.data(), ref_slot, blob::btree_maxreflen);
    }

    {
        blob_t blob(sb->expose_buf().cache()->max_block_size(),
                    ref_buf.data(),
                    blob::btree_maxreflen);
        serialize_onto_blob<cluster_version_t::LATEST_DISK>(
            sb->expose_buf(), &blob, catalog);
    }

    {
        buf_write_t ref_write(&catalog_block);
        char *ref_slot = static_cast<char *>(
            ref_write.get_data_write(blob::btree_maxreflen));
        memcpy(ref_slot, ref_buf.data(), blob::btree_maxreflen);
    }
}

void partition_ops_t::release_catalog_block(
        txn_t *txn, real_superblock_t *sb) {
    guarantee(txn != nullptr);
    guarantee(sb != nullptr);

    block_id_t block_id = sb->get_partition_catalog_block_id();
    if (block_id == NULL_BLOCK_ID) {
        return;
    }

    buf_lock_t catalog_block(sb->expose_buf(), block_id, access_t::write);

    /* Clear the blob tree (deallocates any overflow sub-blocks), then free the
    root catalog block itself. */
    std::vector<char> ref_buf(blob::btree_maxreflen, 0);
    {
        buf_read_t ref_read(&catalog_block);
        uint32_t block_size;
        const char *ref_slot = static_cast<const char *>(
            ref_read.get_data_read(&block_size));
        guarantee(block_size >= static_cast<uint32_t>(blob::btree_maxreflen));
        memcpy(ref_buf.data(), ref_slot, blob::btree_maxreflen);
    }
    {
        blob_t blob(sb->expose_buf().cache()->max_block_size(),
                    ref_buf.data(),
                    blob::btree_maxreflen);
        blob.clear(sb->expose_buf());
    }

    catalog_block.write_acq_signal()->wait_lazily_unordered();
    catalog_block.mark_deleted();
    sb->set_partition_catalog_block_id(NULL_BLOCK_ID);
}

void partition_ops_t::create_partition_stores(
        const partition_config_t &config,
        const table_config_t &table_config,
        partition_catalog_t *catalog,
        signal_t *interruptor) {
    guarantee(catalog != nullptr);
    if (interruptor != nullptr && interruptor->is_pulsed()) {
        throw interrupted_exc_t();
    }

    /* Reject unpartitioned / empty configs; validate coverage, names, bounds. */
    config.validate_or_throw();
    if (!config.is_partitioned()) {
        throw cannot_perform_query_exc_t(
            "create_partition_stores requires a partitioned config",
            query_state_t::FAILED);
    }

    /* Shard count for per-partition superblock root slots. Unsharded tables
    still get one NULL root slot so the vector is never empty for an active
    candidate. */
    size_t num_shards = table_config.shards.size();
    if (num_shards == 0) {
        num_shards = 1;
    }

    /* sindex inheritance is recorded by reference only: every active partition
    store will receive a copy of table_config.sindexes when its superblocks are
    materialised (PART-07). Touching the map here keeps the parameter live and
    documents the requirement for future allocation. */
    const size_t inherited_sindex_count = table_config.sindexes.size();
    (void)inherited_sindex_count;

    partition_catalog_t provisional;
    provisional.format_version = PARTITION_CATALOG_FORMAT_VERSION;
    provisional.epoch = config.epoch;
    provisional.primary_key_directory_block = NULL_BLOCK_ID;
    provisional.stores.reserve(config.partitions.size());

    for (const partition_entry_t &entry : config.partitions) {
        if (interruptor != nullptr && interruptor->is_pulsed()) {
            throw interrupted_exc_t();
        }

        partition_store_ref_t store;
        store.partition_id = entry.id.is_nil() ? generate_uuid() : entry.id;
        store.storage_id =
            entry.storage_id.is_nil() ? generate_uuid() : entry.storage_id;
        store.shard_superblocks.assign(num_shards, NULL_BLOCK_ID);
        /* Invisible to queries until CATCHING_UP / ACTIVE cutover. */
        store.state = partition_state_t::CREATING;
        store.epoch = config.epoch;
        provisional.stores.push_back(std::move(store));
    }

    *catalog = std::move(provisional);
}

void partition_ops_t::move_row_between_partitions(
        const partition_route_t &source,
        const partition_route_t &destination,
        const store_key_t &primary_key,
        const ql::datum_t &new_value,
        signal_t *interruptor) {
    (void)source;
    (void)destination;
    (void)primary_key;
    (void)new_value;
    (void)interruptor;
    /* Full atomic move protocol is PART-07 / PART-08. */
    throw cannot_perform_query_exc_t("unimplemented", query_state_t::FAILED);
}

void partition_ops_t::retire_drained_stores(
        uint64_t minimum_live_epoch,
        partition_catalog_t *catalog) {
    guarantee(catalog != nullptr);

    std::vector<partition_store_ref_t> survivors;
    survivors.reserve(catalog->stores.size());

    for (partition_store_ref_t &store : catalog->stores) {
        const bool is_failed = (store.state == partition_state_t::FAILED);
        const bool is_drained_and_stale =
            (store.state == partition_state_t::DRAINING &&
             store.epoch < minimum_live_epoch);

        if (is_failed || is_drained_and_stale) {
            /* Catalog-level release: clear root refs and drop the entry.
            Physical B-tree block free is performed by the drop path that holds
            a txn / superblock (PART-07). */
            store.shard_superblocks.clear();
            continue;
        }
        survivors.push_back(std::move(store));
    }

    catalog->stores = std::move(survivors);
}
