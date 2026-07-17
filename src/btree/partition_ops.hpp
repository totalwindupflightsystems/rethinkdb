// Copyright 2026 RethinkDB, all rights reserved.
#ifndef BTREE_PARTITION_OPS_HPP_
#define BTREE_PARTITION_OPS_HPP_

/* Operations for the table-level partition catalog blob stored under the primary
B-tree superblock (Phase 3 declarative partitioning).

The catalog lives in a child block referenced by
`real_superblock_t::partition_catalog_block`. The first `blob::btree_maxreflen`
bytes of that block hold a blob ref; the blob tree holds a
`cluster_version_t::LATEST_DISK` serialization of `partition_catalog_t`.

`NULL_BLOCK_ID` means "no catalog published" (unpartitioned table, or an
unpublished candidate). */

#include "btree/reql_specific.hpp"

class txn_t;

class partition_ops_t {
public:
    /* Load the partition catalog from the superblock. If no catalog block is
    present (`NULL_BLOCK_ID`), returns a default-constructed empty catalog. */
    static partition_catalog_t load_catalog(txn_t *txn, real_superblock_t *sb);

    /* Serialize `catalog` into the partition-catalog blob block. Allocates a
    fresh child block when none exists; overwrites (clear + rewrite) when one
    already does. Updates `real_superblock_t::partition_catalog_block`. */
    static void save_catalog(txn_t *txn, real_superblock_t *sb,
                             const partition_catalog_t &catalog);

    /* Clear the catalog blob tree, mark the child block deleted, and set the
    superblock reference to `NULL_BLOCK_ID`. No-op if already absent. */
    static void release_catalog_block(txn_t *txn, real_superblock_t *sb);

private:
    /* Allocate a new empty catalog block under `sb` and return its lock with
    a zeroed blob-ref slot. Caller owns the lock. */
    static buf_lock_t allocate_catalog_block(real_superblock_t *sb);
};

#endif  // BTREE_PARTITION_OPS_HPP_
