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
unpublished candidate).

Lifecycle (section 5.3):
  CREATING → CATCHING_UP → ACTIVE → DRAINING → (retired)
  CREATING / CATCHING_UP → FAILED → (retired)

`partition_ops_t` owns storage-side catalog work and must not parse user ReQL.
Ordinary row writes are NOT serialized by the lifecycle mutex; only repartition
transitions are. */

#include <string>

#include "btree/reql_specific.hpp"
#include "concurrency/new_mutex.hpp"
#include "rdb_protocol/partition_config.hpp"

class txn_t;
class table_config_t;
class signal_t;

/* Per-table mutex that serializes repartition lifecycle transitions (create /
catch-up / cutover / drain / drop). One instance lives with the table manager
for that table. It deliberately does NOT lock ordinary row writes: concurrent
mutations continue against the source epoch while a candidate is built. */
class partition_lifecycle_mutex_t {
public:
    partition_lifecycle_mutex_t() { }

    new_mutex_t *get_mutex() {
        return &mutex_;
    }

private:
    new_mutex_t mutex_;
    DISABLE_COPYING(partition_lifecycle_mutex_t);
};

/* Legal partition lifecycle transitions (section 5.3). Returns an empty string
if the transition is legal; otherwise a human-readable error describing the
illegal edge. DRAINING → retired and FAILED → retired are performed by
`retire_drained_stores` (entry removal), not by a state-to-state edge. */
std::string validate_partition_state_transition(
    partition_state_t from, partition_state_t to);

/* Apply a validated state transition to a store ref. Throws
`cannot_perform_query_exc_t` if the edge is illegal. */
void apply_partition_state_transition(
    partition_store_ref_t *store, partition_state_t to);

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

    /* Allocate invisible target stores for every partition in `config`.

    Builds a provisional catalog (out-parameter `*catalog`):
      - one `partition_store_ref_t` per config partition, state = CREATING
      - storage_id taken from the entry or freshly generated
      - shard_superblocks sized to the table shard count, filled with
        `NULL_BLOCK_ID` (invisible / not yet published on disk)
      - catalog epoch = config.epoch, format_version set

    Inherited sindex configs from `table_config` are recorded as a requirement
    for later superblock materialization: at this stage no B-tree blocks are
    allocated (no txn in the signature). Real root allocation and sindex
    installation happen when the stores become ACTIVE (PART-07).

    Caller must hold the per-table `partition_lifecycle_mutex_t`. */
    static void create_partition_stores(
        const partition_config_t &config,
        const table_config_t &table_config,
        partition_catalog_t *catalog,
        signal_t *interruptor);

    /* Cross-partition primary-key move (PART-07). Skeleton for PART-06 —
    throws `cannot_perform_query_exc_t("unimplemented")`. */
    static void move_row_between_partitions(
        const partition_route_t &source,
        const partition_route_t &destination,
        const store_key_t &primary_key,
        const ql::datum_t &new_value,
        signal_t *interruptor);

    /* Retire stores that are safe to drop from the catalog:
      - DRAINING stores whose epoch is strictly below `minimum_live_epoch`
      - FAILED stores (allocation/build/replay failure)

    Clears shard_superblock references and removes the entries from
    `catalog->stores`. Actual B-tree block free requires a txn and is deferred
    to the drop path that holds a superblock (PART-07); this method is the
    catalog-level tombstone / release step. */
    static void retire_drained_stores(
        uint64_t minimum_live_epoch,
        partition_catalog_t *catalog);

private:
    /* Allocate a new empty catalog block under `sb` and return its lock with
    a zeroed blob-ref slot. Caller owns the lock. */
    static buf_lock_t allocate_catalog_block(real_superblock_t *sb);
};

#endif  // BTREE_PARTITION_OPS_HPP_
