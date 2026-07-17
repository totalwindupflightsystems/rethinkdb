// Copyright 2026 RethinkDB, all rights reserved.
#ifndef BTREE_PARTITION_REPLAY_HPP_
#define BTREE_PARTITION_REPLAY_HPP_

/* Online repartitioning pipeline (Phase 3 / section 5.3).

Implements the 9-step lifecycle:

  1. Validate candidate map
  2. Acquire per-table transition lock; record source epoch E
  3. Allocate invisible target stores (create_partition_stores)
  4. Copy source rows under a consistent snapshot
  5. Replay source modification queue through a high-water stamp
  6. Verify targets caught up
  7. Commit cutover (catalog epoch E+1; Raft set_partition_config_t)
  8. New requests route to E+1; old readers keep source leases
  9. Retire draining sources after leases/queues/feeds complete

Crash before step 7: candidates unpublished and collectable (FAILED + retire).
Crash after step 7: recover cleanup from the durable new epoch.

Storage-side work only — does not parse user ReQL. The Raft proposal itself is
built as `set_partition_config_t` and applied by the table manager; this module
prepares provisional stores and advances the local catalog. */

#include <map>
#include <utility>
#include <vector>

#include "btree/keys.hpp"
#include "btree/partition_ops.hpp"
#include "btree/reql_specific.hpp"
#include "clustering/administration/tables/table_metadata.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/partition_config.hpp"

class txn_t;
class signal_t;

/* In-memory target row state used during snapshot+replay. Keyed by primary
key; value carries the latest applied document and mutation stamp for
idempotent replay (skip if existing stamp >= incoming). */
struct partition_replay_row_t {
    ql::datum_t value;
    uint64_t mutation_stamp;

    partition_replay_row_t()
        : mutation_stamp(0) { }
    partition_replay_row_t(ql::datum_t v, uint64_t s)
        : value(std::move(v)), mutation_stamp(s) { }
};

class partition_replay_t {
public:
    /* Execute the full online repartition pipeline.
    Returns true on successful cutover (new epoch committed locally).
    Returns false on failure (source epoch stays authoritative; unpublished
    candidates cleaned up). */
    static bool repartition(
        const partition_config_t &candidate,
        txn_t *txn,
        real_superblock_t *sb,
        partition_lifecycle_mutex_t *mutex,
        signal_t *interruptor);

    /* Build the Raft-side change for step 7. `expected_epoch` is the source
    epoch E; `new_config` has epoch E+1 and ACTIVE partitions; provisional
    stores are the CATCHING_UP/ACTIVE targets from the catalog. */
    static table_config_and_shards_change_t::set_partition_config_t
    make_cutover_change(
        uint64_t expected_epoch,
        const partition_config_t &new_config,
        const partition_catalog_t &catalog);

    /* Idempotent apply of one transition modification into an in-memory
    target map. Returns true if the row was written/updated, false if skipped
    because a newer-or-equal stamp already exists. Exposed for unit tests. */
    static bool apply_modification_idempotent(
        std::map<store_key_t, partition_replay_row_t> *target_rows,
        const transition_modification_t &mod);

    /* Pure catalog-level abort: mark non-source CREATING/CATCHING_UP stores
    FAILED, retire them, clear the transition queue. Source epoch unchanged.
    Used by fail paths and unit tests simulating a pre-cutover crash. */
    static void abort_unpublished_transition(partition_catalog_t *catalog);

    /* Pure catalog-level cutover (step 7 storage half):
      - source ACTIVE stores at epoch E → DRAINING
      - target CATCHING_UP stores at epoch E+1 → ACTIVE
      - catalog.epoch = E+1
      - clear transition queue / flags
    Returns false if preconditions fail (no active transition, wrong epoch). */
    static bool commit_catalog_cutover(
        partition_catalog_t *catalog,
        uint64_t expected_source_epoch);

private:
    /* Step 4: Copy source rows from current epoch into target stores.
    Records high_water_mark = last assigned stamp after the snapshot. Full
    B-tree row copy lands in PART-08; here we freeze the stamp frontier and
    advance targets CREATING → CATCHING_UP. */
    static void copy_snapshot(
        const partition_catalog_t &source_catalog,
        const partition_config_t &candidate,
        partition_catalog_t *target_catalog,
        std::map<store_key_t, partition_replay_row_t> *target_rows,
        signal_t *interruptor);

    /* Step 5: Replay queued mutations against target stores up to
    high_water_mark. Idempotent by primary key + mutation stamp. */
    static void replay_modifications(
        const partition_catalog_t &source_catalog,
        partition_catalog_t *target_catalog,
        std::map<store_key_t, partition_replay_row_t> *target_rows,
        uint64_t high_water_mark,
        signal_t *interruptor);

    /* Step 6: Verify target indexes + PK directory have caught up.
    Returns true if every queue entry with stamp <= high_water_mark is
    reflected in target_rows at an equal-or-newer stamp. */
    static bool targets_caught_up(
        const partition_catalog_t &target_catalog,
        const std::map<store_key_t, partition_replay_row_t> &target_rows,
        uint64_t high_water_mark);
};

#endif  // BTREE_PARTITION_REPLAY_HPP_
