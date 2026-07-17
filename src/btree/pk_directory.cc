// Copyright 2026 RethinkDB, all rights reserved.
#include "btree/pk_directory.hpp"

#include <string.h>

#include <string>
#include <utility>
#include <vector>

#include "buffer_cache/blob.hpp"
#include "buffer_cache/serialize_onto_blob.hpp"
#include "containers/archive/buffer_group_stream.hpp"
#include "containers/archive/stl_types.hpp"
#include "containers/archive/versioned.hpp"
#include "errors.hpp"
#include "protocol_api.hpp"
#include "utils.hpp"

RDB_IMPL_SERIALIZABLE_2_SINCE_v2_4(
    pk_directory_entry_t, pk, partition_id);
RDB_IMPL_SERIALIZABLE_2_SINCE_v2_4(
    pk_directory_blob_t, format_version, entries);

buf_lock_t pk_directory_t::allocate_dir_block(buf_parent_t parent) {
    /* Child block under the table superblock. First btree_maxreflen bytes
    hold the blob ref (zeroed so blob_t starts from an empty ref). */
    buf_lock_t block(parent, alt_create_t::create);
    {
        buf_write_t write(&block);
        char *ref_slot = static_cast<char *>(
            write.get_data_write(blob::btree_maxreflen));
        memset(ref_slot, 0, blob::btree_maxreflen);
    }
    return block;
}

void pk_directory_t::init(txn_t *txn, buf_parent_t parent,
                          block_id_t *dir_block_id) {
    guarantee(txn != nullptr);
    guarantee(dir_block_id != nullptr);

    if (*dir_block_id != NULL_BLOCK_ID) {
        /* Already allocated — leave existing contents intact. */
        return;
    }

    buf_lock_t block = allocate_dir_block(parent);
    *dir_block_id = block.block_id();

    pk_directory_blob_t empty;
    empty.format_version = PK_DIRECTORY_FORMAT_VERSION;
    save(txn, parent, *dir_block_id, empty);
}

void pk_directory_t::ensure_allocated(txn_t *txn, buf_parent_t parent,
                                      block_id_t *dir_block_id) {
    guarantee(dir_block_id != nullptr);
    if (*dir_block_id == NULL_BLOCK_ID) {
        init(txn, parent, dir_block_id);
    }
}

pk_directory_blob_t pk_directory_t::load(txn_t *txn, buf_parent_t parent,
                                         block_id_t dir_block_id) {
    guarantee(txn != nullptr);
    pk_directory_blob_t out;
    out.format_version = PK_DIRECTORY_FORMAT_VERSION;

    if (dir_block_id == NULL_BLOCK_ID) {
        return out;
    }

    buf_lock_t dir_block(parent, dir_block_id, access_t::read);

    std::vector<char> ref_buf(blob::btree_maxreflen, 0);
    {
        buf_read_t ref_read(&dir_block);
        uint32_t block_size;
        const char *ref_slot = static_cast<const char *>(
            ref_read.get_data_read(&block_size));
        guarantee(block_size >= static_cast<uint32_t>(blob::btree_maxreflen));
        memcpy(ref_buf.data(), ref_slot, blob::btree_maxreflen);
    }

    blob_t blob(parent.cache()->max_block_size(),
                ref_buf.data(),
                blob::btree_maxreflen);

    if (blob.valuesize() == 0) {
        return out;
    }

    {
        buffer_group_t buffer_group;
        blob_acq_t acq;
        blob.expose_all(parent, access_t::read, &buffer_group, &acq);
        deserialize_from_group<cluster_version_t::LATEST_DISK>(
            const_view(&buffer_group), &out);
    }
    return out;
}

void pk_directory_t::save(txn_t *txn, buf_parent_t parent,
                          block_id_t dir_block_id,
                          const pk_directory_blob_t &blob_data) {
    guarantee(txn != nullptr);
    guarantee(dir_block_id != NULL_BLOCK_ID);

    buf_lock_t dir_block(parent, dir_block_id, access_t::write);

    std::vector<char> ref_buf(blob::btree_maxreflen, 0);
    {
        buf_read_t ref_read(&dir_block);
        uint32_t block_size;
        const char *ref_slot = static_cast<const char *>(
            ref_read.get_data_read(&block_size));
        guarantee(block_size >= static_cast<uint32_t>(blob::btree_maxreflen));
        memcpy(ref_buf.data(), ref_slot, blob::btree_maxreflen);
    }

    {
        blob_t blob(parent.cache()->max_block_size(),
                    ref_buf.data(),
                    blob::btree_maxreflen);
        serialize_onto_blob<cluster_version_t::LATEST_DISK>(
            parent, &blob, blob_data);
    }

    {
        buf_write_t ref_write(&dir_block);
        char *ref_slot = static_cast<char *>(
            ref_write.get_data_write(blob::btree_maxreflen));
        memcpy(ref_slot, ref_buf.data(), blob::btree_maxreflen);
    }
}

void pk_directory_t::release(txn_t *txn, buf_parent_t parent,
                             block_id_t *dir_block_id) {
    guarantee(txn != nullptr);
    guarantee(dir_block_id != nullptr);

    if (*dir_block_id == NULL_BLOCK_ID) {
        return;
    }

    buf_lock_t dir_block(parent, *dir_block_id, access_t::write);

    std::vector<char> ref_buf(blob::btree_maxreflen, 0);
    {
        buf_read_t ref_read(&dir_block);
        uint32_t block_size;
        const char *ref_slot = static_cast<const char *>(
            ref_read.get_data_read(&block_size));
        guarantee(block_size >= static_cast<uint32_t>(blob::btree_maxreflen));
        memcpy(ref_buf.data(), ref_slot, blob::btree_maxreflen);
    }
    {
        blob_t blob(parent.cache()->max_block_size(),
                    ref_buf.data(),
                    blob::btree_maxreflen);
        blob.clear(parent);
    }

    dir_block.write_acq_signal()->wait_lazily_unordered();
    dir_block.mark_deleted();
    *dir_block_id = NULL_BLOCK_ID;
}

bool pk_directory_t::try_insert(txn_t *txn, buf_parent_t parent,
                                block_id_t *dir_block_id,
                                const store_key_t &pk,
                                const uuid_u &partition_id) {
    guarantee(txn != nullptr);
    guarantee(dir_block_id != nullptr);
    guarantee(!partition_id.is_nil());

    ensure_allocated(txn, parent, dir_block_id);

    pk_directory_blob_t blob = load(txn, parent, *dir_block_id);
    for (const pk_directory_entry_t &e : blob.entries) {
        if (e.pk == pk) {
            return false;
        }
    }

    blob.format_version = PK_DIRECTORY_FORMAT_VERSION;
    blob.entries.emplace_back(pk, partition_id);
    save(txn, parent, *dir_block_id, blob);
    return true;
}

uuid_u pk_directory_t::lookup(txn_t *txn, buf_parent_t parent,
                              block_id_t dir_block_id,
                              const store_key_t &pk) {
    guarantee(txn != nullptr);

    if (dir_block_id == NULL_BLOCK_ID) {
        return nil_uuid();
    }

    pk_directory_blob_t blob = load(txn, parent, dir_block_id);
    for (const pk_directory_entry_t &e : blob.entries) {
        if (e.pk == pk) {
            return e.partition_id;
        }
    }
    return nil_uuid();
}

bool pk_directory_t::exists(txn_t *txn, buf_parent_t parent,
                            block_id_t dir_block_id,
                            const store_key_t &pk) {
    return !lookup(txn, parent, dir_block_id, pk).is_nil();
}

void pk_directory_t::remove(txn_t *txn, buf_parent_t parent,
                            block_id_t *dir_block_id,
                            const store_key_t &pk) {
    guarantee(txn != nullptr);
    guarantee(dir_block_id != nullptr);

    if (*dir_block_id == NULL_BLOCK_ID) {
        return;
    }

    pk_directory_blob_t blob = load(txn, parent, *dir_block_id);
    std::vector<pk_directory_entry_t> survivors;
    survivors.reserve(blob.entries.size());
    bool removed = false;
    for (pk_directory_entry_t &e : blob.entries) {
        if (!removed && e.pk == pk) {
            removed = true;
            continue;
        }
        survivors.push_back(std::move(e));
    }
    if (!removed) {
        return;
    }
    blob.entries = std::move(survivors);
    blob.format_version = PK_DIRECTORY_FORMAT_VERSION;
    save(txn, parent, *dir_block_id, blob);
}

void pk_directory_t::move_entry(txn_t *txn, buf_parent_t parent,
                                block_id_t *dir_block_id,
                                const store_key_t &pk,
                                const uuid_u &from_partition,
                                const uuid_u &to_partition) {
    guarantee(txn != nullptr);
    guarantee(dir_block_id != nullptr);
    guarantee(!from_partition.is_nil());
    guarantee(!to_partition.is_nil());

    if (from_partition == to_partition) {
        /* Same partition — no directory change required, but the PK must
        already be owned by that partition for a consistent move fence. */
        uuid_u owner = lookup(txn, parent, *dir_block_id, pk);
        if (owner.is_nil() || owner != from_partition) {
            throw cannot_perform_query_exc_t(
                "partition move failed: primary key not owned by source partition",
                query_state_t::FAILED);
        }
        return;
    }

    ensure_allocated(txn, parent, dir_block_id);

    pk_directory_blob_t blob = load(txn, parent, *dir_block_id);
    pk_directory_entry_t *found = nullptr;
    for (pk_directory_entry_t &e : blob.entries) {
        if (e.pk == pk) {
            found = &e;
            break;
        }
    }

    if (found == nullptr || found->partition_id != from_partition) {
        /* Source is not authoritative — leave directory untouched. */
        throw cannot_perform_query_exc_t(
            "partition move failed: primary key not owned by source partition",
            query_state_t::FAILED);
    }

    /* Atomic ownership swap in the single-blob rewrite. Source stays
    authoritative until this save completes; there is no intermediate
    "unowned" or dual-ownership window on disk. */
    found->partition_id = to_partition;
    blob.format_version = PK_DIRECTORY_FORMAT_VERSION;
    save(txn, parent, *dir_block_id, blob);
}

void pk_directory_t::remove_all_for_partition(txn_t *txn, buf_parent_t parent,
                                              block_id_t *dir_block_id,
                                              const uuid_u &partition_id) {
    guarantee(txn != nullptr);
    guarantee(dir_block_id != nullptr);

    if (*dir_block_id == NULL_BLOCK_ID || partition_id.is_nil()) {
        return;
    }

    pk_directory_blob_t blob = load(txn, parent, *dir_block_id);
    std::vector<pk_directory_entry_t> survivors;
    survivors.reserve(blob.entries.size());
    bool removed_any = false;
    for (pk_directory_entry_t &e : blob.entries) {
        if (e.partition_id == partition_id) {
            removed_any = true;
            continue;
        }
        survivors.push_back(std::move(e));
    }
    if (!removed_any) {
        return;
    }
    blob.entries = std::move(survivors);
    blob.format_version = PK_DIRECTORY_FORMAT_VERSION;
    save(txn, parent, *dir_block_id, blob);
}
