// Copyright 2026 RethinkDB, all rights reserved.
#include "btree/partition_ops.hpp"

#include <string.h>

#include <vector>

#include "buffer_cache/alt.hpp"
#include "buffer_cache/blob.hpp"
#include "buffer_cache/serialize_onto_blob.hpp"
#include "containers/archive/buffer_group_stream.hpp"
#include "containers/archive/versioned.hpp"

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
