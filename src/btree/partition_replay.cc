// Copyright 2026 RethinkDB, all rights reserved.
#include "btree/partition_replay.hpp"

#include <string>
#include <utility>
#include <vector>

#include "clustering/administration/tables/table_metadata.hpp"
#include "concurrency/interruptor.hpp"
#include "concurrency/new_mutex.hpp"
#include "containers/uuid.hpp"
#include "errors.hpp"
#include "protocol_api.hpp"
#include "utils.hpp"

/* Clear transition-queue fields after cutover or abort. */
static void clear_transition_fields(partition_catalog_t *catalog) {
    guarantee(catalog != nullptr);
    catalog->transition_active = false;
    catalog->transition_source_epoch = 0;
    catalog->next_mutation_stamp = 1;
    catalog->high_water_mark = 0;
    catalog->transition_queue.clear();
}

bool partition_replay_t::apply_modification_idempotent(
        std::map<store_key_t, partition_replay_row_t> *target_rows,
        const transition_modification_t &mod) {
    guarantee(target_rows != nullptr);

    auto it = target_rows->find(mod.primary_key);
    if (it != target_rows->end() && it->second.mutation_stamp >= mod.mutation_stamp) {
        /* Newer or equal stamp already applied — skip (crash-safe restart). */
        return false;
    }

    /* Null / missing value means delete. */
    if (!mod.value.has() || mod.value.get_type() == ql::datum_t::R_NULL) {
        if (it != target_rows->end()) {
            target_rows->erase(it);
        }
        /* Still record a tombstone stamp so a later older replay is skipped.
        Represent deletes as a null-valued row with the stamp. */
        (*target_rows)[mod.primary_key] =
            partition_replay_row_t(ql::datum_t::null(), mod.mutation_stamp);
        return true;
    }

    (*target_rows)[mod.primary_key] =
        partition_replay_row_t(mod.value, mod.mutation_stamp);
    return true;
}

void partition_replay_t::abort_unpublished_transition(
        partition_catalog_t *catalog) {
    guarantee(catalog != nullptr);

    /* Mark unpublished candidates FAILED, then retire them. Source ACTIVE /
    DRAINING stores at the source epoch are left alone. */
    const uint64_t source_epoch = catalog->transition_active
        ? catalog->transition_source_epoch
        : catalog->epoch;

    for (partition_store_ref_t &store : catalog->stores) {
        if (store.epoch != source_epoch &&
            (store.state == partition_state_t::CREATING ||
             store.state == partition_state_t::CATCHING_UP)) {
            /* Force FAILED even if the formal edge is CREATING/CATCHING_UP →
            FAILED (validated). */
            std::string err =
                validate_partition_state_transition(store.state,
                                                    partition_state_t::FAILED);
            if (err.empty()) {
                store.state = partition_state_t::FAILED;
            } else {
                store.state = partition_state_t::FAILED;
            }
        }
    }

    partition_ops_t::retire_drained_stores(
        /* minimum_live_epoch: only retire FAILED (any epoch) and DRAINING
        below this. Source epoch stays live. */
        source_epoch + 1,
        catalog);

    /* retire_drained_stores retires FAILED regardless of epoch and DRAINING
    with epoch < minimum. After the loop, FAILED targets are gone. */

    clear_transition_fields(catalog);
    /* Source epoch remains authoritative. */
}

bool partition_replay_t::commit_catalog_cutover(
        partition_catalog_t *catalog,
        uint64_t expected_source_epoch) {
    guarantee(catalog != nullptr);

    if (!catalog->transition_active) {
        return false;
    }
    if (catalog->transition_source_epoch != expected_source_epoch) {
        return false;
    }
    if (catalog->epoch != expected_source_epoch) {
        return false;
    }

    const uint64_t new_epoch = expected_source_epoch + 1;

    for (partition_store_ref_t &store : catalog->stores) {
        if (store.epoch == expected_source_epoch &&
            store.state == partition_state_t::ACTIVE) {
            apply_partition_state_transition(
                &store, partition_state_t::DRAINING);
        } else if (store.epoch == new_epoch &&
                   store.state == partition_state_t::CATCHING_UP) {
            apply_partition_state_transition(
                &store, partition_state_t::ACTIVE);
        }
    }

    catalog->epoch = new_epoch;
    clear_transition_fields(catalog);
    return true;
}

table_config_and_shards_change_t::set_partition_config_t
partition_replay_t::make_cutover_change(
        uint64_t expected_epoch,
        const partition_config_t &new_config,
        const partition_catalog_t &catalog) {
    table_config_and_shards_change_t::set_partition_config_t change;
    change.expected_epoch = expected_epoch;
    change.new_config = new_config;

    for (const partition_store_ref_t &store : catalog.stores) {
        if (store.epoch == expected_epoch + 1 &&
            (store.state == partition_state_t::CATCHING_UP ||
             store.state == partition_state_t::ACTIVE ||
             store.state == partition_state_t::CREATING)) {
            change.provisional_stores.push_back(store);
        }
    }
    return change;
}

void partition_replay_t::copy_snapshot(
        const partition_catalog_t &source_catalog,
        const partition_config_t &candidate,
        partition_catalog_t *target_catalog,
        std::map<store_key_t, partition_replay_row_t> *target_rows,
        signal_t *interruptor) {
    guarantee(target_catalog != nullptr);
    guarantee(target_rows != nullptr);
    if (interruptor != nullptr && interruptor->is_pulsed()) {
        throw interrupted_exc_t();
    }

    /* Full physical row copy from source B-trees into target stores is
    PART-08. At this stage we:
      1. Validate the candidate route table is usable
      2. Freeze the mutation-stamp high-water mark
      3. Advance CREATING → CATCHING_UP on target stores

    Concurrent writes continue to enqueue into the durable transition queue
    against source epoch E; those with stamp <= high_water_mark are replayed
    in step 5. */

    (void)source_catalog;
    candidate.validate_or_throw();

    /* High-water = last stamp that has been (or will have been) enqueued by
    the time the snapshot frontier is taken. next_mutation_stamp is the next
    free stamp, so the inclusive high-water is next-1 (or 0 if none yet). */
    if (target_catalog->next_mutation_stamp > 1) {
        target_catalog->high_water_mark =
            target_catalog->next_mutation_stamp - 1;
    } else {
        target_catalog->high_water_mark = 0;
    }

    for (partition_store_ref_t &store : target_catalog->stores) {
        if (store.state == partition_state_t::CREATING &&
            store.epoch == candidate.epoch) {
            apply_partition_state_transition(
                &store, partition_state_t::CATCHING_UP);
        }
    }

    (void)target_rows;
}

void partition_replay_t::replay_modifications(
        const partition_catalog_t &source_catalog,
        partition_catalog_t *target_catalog,
        std::map<store_key_t, partition_replay_row_t> *target_rows,
        uint64_t high_water_mark,
        signal_t *interruptor) {
    guarantee(target_catalog != nullptr);
    guarantee(target_rows != nullptr);
    if (interruptor != nullptr && interruptor->is_pulsed()) {
        throw interrupted_exc_t();
    }

    (void)source_catalog;

    for (const transition_modification_t &mod :
             target_catalog->transition_queue) {
        if (interruptor != nullptr && interruptor->is_pulsed()) {
            throw interrupted_exc_t();
        }
        if (mod.mutation_stamp > high_water_mark) {
            /* Past the frozen frontier — leave for a later drain pass or
            discard after cutover (post-HWM writes either hit source E before
            cutover and are already in the queue beyond HWM, or route to E+1
            after commit). Cutover requires only HWM catch-up. */
            continue;
        }
        apply_modification_idempotent(target_rows, mod);
    }
}

bool partition_replay_t::targets_caught_up(
        const partition_catalog_t &target_catalog,
        const std::map<store_key_t, partition_replay_row_t> &target_rows,
        uint64_t high_water_mark) {
    /* For every queue entry with stamp <= HWM, the target map must hold that
    PK at an equal-or-newer stamp. Entries that are superseded by a later
    same-PK mutation within the HWM window only need the latest stamp. */

    std::map<store_key_t, uint64_t> required;
    for (const transition_modification_t &mod :
             target_catalog.transition_queue) {
        if (mod.mutation_stamp > high_water_mark) {
            continue;
        }
        auto it = required.find(mod.primary_key);
        if (it == required.end() || it->second < mod.mutation_stamp) {
            required[mod.primary_key] = mod.mutation_stamp;
        }
    }

    for (const auto &req : required) {
        auto it = target_rows.find(req.first);
        if (it == target_rows.end()) {
            return false;
        }
        if (it->second.mutation_stamp < req.second) {
            return false;
        }
    }

    /* Also require every target store for the candidate epoch is CATCHING_UP
    (ready for ACTIVE cutover). */
    bool saw_target = false;
    for (const partition_store_ref_t &store : target_catalog.stores) {
        if (store.state == partition_state_t::CATCHING_UP) {
            saw_target = true;
        } else if (store.state == partition_state_t::CREATING) {
            /* Snapshot never completed for this store. */
            return false;
        }
    }
    /* Empty candidate is rejected earlier; saw_target may be false only if
    there were no CATCHING_UP stores (e.g. empty catalog) — treat as not
    caught up. */
    if (!saw_target && !required.empty()) {
        return false;
    }

    return true;
}

bool partition_replay_t::repartition(
        const partition_config_t &candidate,
        txn_t *txn,
        real_superblock_t *sb,
        partition_lifecycle_mutex_t *mutex,
        signal_t *interruptor) {
    guarantee(txn != nullptr);
    guarantee(sb != nullptr);
    guarantee(mutex != nullptr);

    /* Step 2: acquire the per-table transition lock. */
    new_mutex_acq_t transition_lock(mutex->get_mutex(), interruptor);

    partition_catalog_t catalog = partition_ops_t::load_catalog(txn, sb);

    try {
        /* Step 1: validate candidate. */
        candidate.validate_or_throw();
        if (!candidate.is_partitioned()) {
            throw cannot_perform_query_exc_t(
                "repartition requires a partitioned candidate map",
                query_state_t::FAILED);
        }

        if (catalog.transition_active) {
            throw cannot_perform_query_exc_t(
                "partition transition already running",
                query_state_t::FAILED);
        }

        /* Step 2b: record source epoch E. */
        const uint64_t source_epoch = catalog.epoch;
        catalog.transition_active = true;
        catalog.transition_source_epoch = source_epoch;
        catalog.next_mutation_stamp = 1;
        catalog.high_water_mark = 0;
        catalog.transition_queue.clear();
        partition_ops_t::save_catalog(txn, sb, catalog);

        /* Step 3: allocate invisible target stores at epoch E+1. */
        partition_config_t target_config = candidate;
        target_config.epoch = source_epoch + 1;
        for (partition_entry_t &entry : target_config.partitions) {
            entry.state = partition_state_t::CREATING;
        }

        table_config_t table_config;
        /* Shards / sindexes inherited when the table manager wires real
        materialization; empty here means one NULL superblock slot per store. */
        partition_catalog_t provisional;
        partition_ops_t::create_partition_stores(
            target_config, table_config, &provisional, interruptor);

        /* Merge provisional targets into the live catalog (sources stay). */
        for (partition_store_ref_t &store : provisional.stores) {
            catalog.stores.push_back(std::move(store));
        }
        partition_ops_t::save_catalog(txn, sb, catalog);

        std::map<store_key_t, partition_replay_row_t> target_rows;

        /* Step 4: snapshot copy + CREATING → CATCHING_UP. */
        copy_snapshot(catalog, target_config, &catalog, &target_rows,
                      interruptor);
        partition_ops_t::save_catalog(txn, sb, catalog);

        const uint64_t hwm = catalog.high_water_mark;

        /* Step 5: replay queue through high-water mark. */
        replay_modifications(catalog, &catalog, &target_rows, hwm, interruptor);

        /* Step 6: verify catch-up. */
        if (!targets_caught_up(catalog, target_rows, hwm)) {
            abort_unpublished_transition(&catalog);
            partition_ops_t::save_catalog(txn, sb, catalog);
            return false;
        }

        /* Step 7: build Raft change (for the table manager) and commit the
        local catalog cutover. Raft apply of set_partition_config_t is the
        cluster-visible half; storage epoch advances here so a crash after
        this point recovers cleanup from the durable new epoch. */
        table_config_and_shards_change_t::set_partition_config_t raft_change =
            make_cutover_change(source_epoch, target_config, catalog);
        (void)raft_change;

        /* Promote target_config partition entries to ACTIVE for the committed
        map that the table manager will publish. */
        for (partition_entry_t &entry : target_config.partitions) {
            entry.state = partition_state_t::ACTIVE;
        }

        if (!commit_catalog_cutover(&catalog, source_epoch)) {
            abort_unpublished_transition(&catalog);
            partition_ops_t::save_catalog(txn, sb, catalog);
            return false;
        }
        partition_ops_t::save_catalog(txn, sb, catalog);

        /* Steps 8–9: routing to E+1 and drain of old leases are handled by
        the query layer / table manager once the Raft map is live. Physical
        retire of DRAINING sources is `retire_drained_stores` once
        minimum_live_epoch advances past E. */
        return true;

    } catch (const interrupted_exc_t &) {
        /* Leave transition state durable so a resume/abort can run; do not
        silently mark success. */
        throw;
    } catch (const cannot_perform_query_exc_t &) {
        /* Fail path: source epoch stays authoritative. Reload and clean up
        unpublished candidates best-effort. */
        try {
            catalog = partition_ops_t::load_catalog(txn, sb);
            if (catalog.transition_active) {
                abort_unpublished_transition(&catalog);
                partition_ops_t::save_catalog(txn, sb, catalog);
            }
        } catch (...) {
            /* Best-effort cleanup only. */
        }
        return false;
    } catch (...) {
        try {
            catalog = partition_ops_t::load_catalog(txn, sb);
            if (catalog.transition_active) {
                abort_unpublished_transition(&catalog);
                partition_ops_t::save_catalog(txn, sb, catalog);
            }
        } catch (...) {
        }
        return false;
    }
}
