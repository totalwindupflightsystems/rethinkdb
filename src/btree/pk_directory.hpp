// Copyright 2026 RethinkDB, all rights reserved.
#ifndef BTREE_PK_DIRECTORY_HPP_
#define BTREE_PK_DIRECTORY_HPP_

/* Global primary-key directory for Phase 3 declarative partitioning.

Maps `store_key_t` (encoded primary key) → `uuid_u` (owning partition UUID)
across all partitions of a logical table. Used for:

  - Duplicate-PK enforcement at insert time
  - Point-read / move routing
  - Atomic cross-partition PK ownership transfer

On-disk layout mirrors the partition catalog: a child block under the table
superblock holds a blob ref in its first `blob::btree_maxreflen` bytes; the
blob tree stores a `cluster_version_t::LATEST_DISK` serialization of a vector
of `{pk, partition_id}` entries. The block ID lives in
`partition_catalog_t::primary_key_directory_block`.

`NULL_BLOCK_ID` means the directory has not been allocated yet (empty). */

#include <vector>

#include "btree/keys.hpp"
#include "buffer_cache/alt.hpp"
#include "containers/uuid.hpp"
#include "rpc/serialize_macros.hpp"

class txn_t;

/* One durable directory mapping. Public fields for serialization macros. */
struct pk_directory_entry_t {
    store_key_t pk;
    uuid_u partition_id;

    pk_directory_entry_t() { }
    pk_directory_entry_t(const store_key_t &k, const uuid_u &pid)
        : pk(k), partition_id(pid) { }
};
RDB_DECLARE_SERIALIZABLE(pk_directory_entry_t);

/* On-disk blob payload for the primary-key directory. */
struct pk_directory_blob_t {
    uint32_t format_version;
    std::vector<pk_directory_entry_t> entries;

    pk_directory_blob_t() : format_version(1) { }
};
RDB_DECLARE_SERIALIZABLE(pk_directory_blob_t);

static constexpr uint32_t PK_DIRECTORY_FORMAT_VERSION = 1;

class pk_directory_t {
public:
    /* Insert a mapping. Returns false if the key already exists (duplicate
    PK). Throws on I/O / serialization error. Allocates the directory block
    when `*dir_block_id == NULL_BLOCK_ID`. */
    static bool try_insert(txn_t *txn, buf_parent_t parent,
                           block_id_t *dir_block_id,
                           const store_key_t &pk,
                           const uuid_u &partition_id);

    /* Look up which partition owns a PK. Returns `nil_uuid()` if not found
    or if the directory block is absent. (Returned by value: the on-disk
    entry is not memory-stable across calls.) */
    static uuid_u lookup(txn_t *txn, buf_parent_t parent,
                         block_id_t dir_block_id,
                         const store_key_t &pk);

    /* Remove a PK mapping (deletes or failed moves). No-op if absent. */
    static void remove(txn_t *txn, buf_parent_t parent,
                       block_id_t *dir_block_id,
                       const store_key_t &pk);

    /* True iff the PK is present in the directory. */
    static bool exists(txn_t *txn, buf_parent_t parent,
                       block_id_t dir_block_id,
                       const store_key_t &pk);

    /* Atomically transfer ownership of `pk` from `from_partition` to
    `to_partition`. Source must currently own the PK; throws
    `cannot_perform_query_exc_t` otherwise (including missing entry). */
    static void move_entry(txn_t *txn, buf_parent_t parent,
                           block_id_t *dir_block_id,
                           const store_key_t &pk,
                           const uuid_u &from_partition,
                           const uuid_u &to_partition);

    /* Drop every entry owned by `partition_id` (drain / drop cleanup). */
    static void remove_all_for_partition(txn_t *txn, buf_parent_t parent,
                                         block_id_t *dir_block_id,
                                         const uuid_u &partition_id);

    /* Allocate a new empty directory block and write an empty payload.
    Sets `*dir_block_id` to the new block. */
    static void init(txn_t *txn, buf_parent_t parent,
                     block_id_t *dir_block_id);

    /* Free the directory blob tree and root block; set `*dir_block_id` to
    `NULL_BLOCK_ID`. No-op if already absent. */
    static void release(txn_t *txn, buf_parent_t parent,
                        block_id_t *dir_block_id);

private:
    static buf_lock_t allocate_dir_block(buf_parent_t parent);

    static pk_directory_blob_t load(txn_t *txn, buf_parent_t parent,
                                    block_id_t dir_block_id);

    static void save(txn_t *txn, buf_parent_t parent,
                     block_id_t dir_block_id,
                     const pk_directory_blob_t &blob);

    /* Ensure `*dir_block_id` is a live directory block; allocates if null. */
    static void ensure_allocated(txn_t *txn, buf_parent_t parent,
                                 block_id_t *dir_block_id);
};

#endif  // BTREE_PK_DIRECTORY_HPP_
