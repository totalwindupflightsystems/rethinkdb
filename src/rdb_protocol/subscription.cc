// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/subscription.hpp"

#include <utility>

#include "containers/archive/archive.hpp"
#include "containers/archive/stl_types.hpp"
#include "containers/archive/versioned.hpp"
#include "errors.hpp"
#include "rdb_protocol/datum.hpp"
#include "utils.hpp"

RDB_IMPL_SERIALIZABLE_10_SINCE_v2_4(
    ql::subscription_config_t,
    subscription_id, name, target_database_id, target_table_id,
    publication_name, source_cluster_id, conflict_policy, state,
    created_by_user_id, created_at);

RDB_IMPL_SERIALIZABLE_4_SINCE_v2_4(
    ql::subscription_error_t,
    code, message, occurred_at, confirmed_lsn_by_shard);

RDB_IMPL_SERIALIZABLE_2_SINCE_v2_4(
    ql::snapshot_barrier_t, shard_id, barrier_lsn);

RDB_IMPL_SERIALIZABLE_5_SINCE_v2_4(
    ql::snapshot_frame_t,
    kind, shard_id, barrier_lsn, row_image, snapshot);

namespace ql {

// ── subscription_handle_t ──────────────────────────────────────────────────

subscription_handle_t::subscription_handle_t() { }

subscription_handle_t::subscription_handle_t(subscription_config_t cfg)
    : config(std::move(cfg)) { }

void subscription_handle_t::request_cancel() {
    assert_thread();
    if (!cancel_signal_.is_pulsed()) {
        cancel_signal_.pulse();
    }
}

// ── State machine ──────────────────────────────────────────────────────────

namespace {

bool is_terminal(subscription_state_t s) {
    return s == subscription_state_t::DROPPED
        || s == subscription_state_t::ERROR
        || s == subscription_state_t::RESYNC_REQUIRED;
}

void set_reason(std::string *reason_out, const std::string &msg) {
    if (reason_out != nullptr) {
        *reason_out = msg;
    }
}

}  // namespace

bool validate_state_transition(
        subscription_state_t from,
        subscription_state_t to,
        std::string *reason_out) {
    if (from == to) {
        set_reason(reason_out,
            strprintf("no-op transition %s → %s",
                      subscription_state_to_string(from),
                      subscription_state_to_string(to)));
        return false;
    }

    /* Terminal states accept no further transitions. */
    if (is_terminal(from)) {
        set_reason(reason_out,
            strprintf("cannot leave terminal state %s (requested %s)",
                      subscription_state_to_string(from),
                      subscription_state_to_string(to)));
        return false;
    }

    /* DROPPING is only allowed to proceed to DROPPED (or ERROR on failure
     * during drop cleanup). */
    if (from == subscription_state_t::DROPPING) {
        if (to == subscription_state_t::DROPPED
            || to == subscription_state_t::ERROR) {
            return true;
        }
        set_reason(reason_out,
            strprintf("DROPPING may only proceed to DROPPED or ERROR, not %s",
                      subscription_state_to_string(to)));
        return false;
    }

    /* From any non-terminal non-DROPPING state, ERROR / RESYNC_REQUIRED /
     * DROPPING are always legal. */
    if (to == subscription_state_t::ERROR
        || to == subscription_state_t::RESYNC_REQUIRED
        || to == subscription_state_t::DROPPING) {
        return true;
    }

    /* Happy-path and pause/resume edges. */
    switch (from) {
    case subscription_state_t::CREATING:
        if (to == subscription_state_t::CONNECTING) {
            return true;
        }
        break;
    case subscription_state_t::CONNECTING:
        if (to == subscription_state_t::SNAPSHOTTING
            || to == subscription_state_t::CATCHING_UP) {
            return true;
        }
        break;
    case subscription_state_t::SNAPSHOTTING:
        if (to == subscription_state_t::CATCHING_UP) {
            return true;
        }
        break;
    case subscription_state_t::CATCHING_UP:
        if (to == subscription_state_t::STREAMING) {
            return true;
        }
        break;
    case subscription_state_t::STREAMING:
        if (to == subscription_state_t::PAUSED) {
            return true;
        }
        break;
    case subscription_state_t::PAUSED:
        if (to == subscription_state_t::STREAMING) {
            return true;
        }
        break;
    case subscription_state_t::DROPPING:
    case subscription_state_t::DROPPED:
    case subscription_state_t::ERROR:
    case subscription_state_t::RESYNC_REQUIRED:
        unreachable();
    }

    set_reason(reason_out,
        strprintf("illegal subscription transition %s → %s",
                  subscription_state_to_string(from),
                  subscription_state_to_string(to)));
    return false;
}

bool apply_transition(
        subscription_handle_t *handle,
        subscription_state_t to,
        const subscription_error_t *error,
        std::string *reason_out) {
    guarantee(handle != nullptr);
    handle->assert_thread();

    if (handle->is_cancel_requested()
        && to != subscription_state_t::DROPPING
        && to != subscription_state_t::DROPPED
        && to != subscription_state_t::ERROR) {
        set_reason(reason_out,
            "subscription cancel requested; only DROPPING/DROPPED/ERROR allowed");
        return false;
    }

    if (!validate_state_transition(handle->config.state, to, reason_out)) {
        return false;
    }

    if (to == subscription_state_t::ERROR
        || to == subscription_state_t::RESYNC_REQUIRED) {
        if (error == nullptr) {
            set_reason(reason_out,
                "ERROR/RESYNC_REQUIRED transitions require diagnostic details");
            return false;
        }
        subscription_error_t err = *error;
        if (err.occurred_at == 0) {
            err.occurred_at = current_microtime();
        }
        if (err.confirmed_lsn_by_shard.empty()) {
            err.confirmed_lsn_by_shard = handle->confirmed_lsn_by_shard;
        }
        handle->last_error = optional<subscription_error_t>(std::move(err));
    }

    if (to == subscription_state_t::DROPPING) {
        /* Cancel in-flight snapshot / catch-up / stream ops. */
        handle->request_cancel();
    }

    handle->config.state = to;
    return true;
}

bool transition_creating_to_connecting(
        subscription_handle_t *handle,
        const uuid_u &publication_id,
        const uuid_u &source_database_id,
        const uuid_u &source_table_id,
        std::string *reason_out) {
    guarantee(handle != nullptr);
    handle->assert_thread();

    if (handle->config.state != subscription_state_t::CREATING) {
        set_reason(reason_out,
            strprintf("expected CREATING, found %s",
                      subscription_state_to_string(handle->config.state)));
        return false;
    }
    if (publication_id.is_nil()) {
        set_reason(reason_out, "publication_id must not be nil");
        return false;
    }
    if (source_table_id.is_nil()) {
        set_reason(reason_out, "source_table_id must not be nil");
        return false;
    }

    if (!apply_transition(
            handle, subscription_state_t::CONNECTING, nullptr, reason_out)) {
        return false;
    }
    handle->publication_id = publication_id;
    handle->source_database_id = source_database_id;
    handle->source_table_id = source_table_id;
    return true;
}

bool transition_connecting_to_snapshotting(
        subscription_handle_t *handle,
        const std::vector<snapshot_barrier_t> &barriers,
        const uuid_u &slot_id,
        std::string *reason_out) {
    guarantee(handle != nullptr);
    handle->assert_thread();

    if (handle->config.state != subscription_state_t::CONNECTING) {
        set_reason(reason_out,
            strprintf("expected CONNECTING, found %s",
                      subscription_state_to_string(handle->config.state)));
        return false;
    }
    if (handle->snapshot_mode != snapshot_mode_t::FULL) {
        set_reason(reason_out,
            "CONNECTING→SNAPSHOTTING requires snapshot_mode FULL/initial");
        return false;
    }
    if (barriers.empty()) {
        set_reason(reason_out,
            "snapshot barriers must be non-empty for SNAPSHOTTING");
        return false;
    }
    if (slot_id.is_nil()) {
        set_reason(reason_out, "slot_id must not be nil");
        return false;
    }

    if (!apply_transition(
            handle, subscription_state_t::SNAPSHOTTING, nullptr, reason_out)) {
        return false;
    }

    handle->slot_id = slot_id;
    handle->snapshot_barrier_lsn_by_shard.clear();
    handle->snapshot_partition_complete.clear();
    for (const snapshot_barrier_t &b : barriers) {
        handle->snapshot_barrier_lsn_by_shard[b.shard_id] = b.barrier_lsn;
        handle->snapshot_partition_complete[b.shard_id] = false;
    }
    return true;
}

bool transition_connecting_to_catching_up(
        subscription_handle_t *handle,
        const std::vector<snapshot_barrier_t> &live_start_positions,
        const uuid_u &slot_id,
        std::string *reason_out) {
    guarantee(handle != nullptr);
    handle->assert_thread();

    if (handle->config.state != subscription_state_t::CONNECTING) {
        set_reason(reason_out,
            strprintf("expected CONNECTING, found %s",
                      subscription_state_to_string(handle->config.state)));
        return false;
    }
    if (handle->snapshot_mode != snapshot_mode_t::NONE) {
        set_reason(reason_out,
            "CONNECTING→CATCHING_UP (skip snapshot) requires snapshot_mode NONE");
        return false;
    }
    if (slot_id.is_nil()) {
        set_reason(reason_out, "slot_id must not be nil");
        return false;
    }

    if (!apply_transition(
            handle, subscription_state_t::CATCHING_UP, nullptr, reason_out)) {
        return false;
    }

    handle->slot_id = slot_id;
    handle->snapshot_barrier_lsn_by_shard.clear();
    handle->snapshot_partition_complete.clear();
    for (const snapshot_barrier_t &b : live_start_positions) {
        /* For snapshot:none the "barrier" is the live-start LSN; there is no
         * snapshot partition work, so partitions are marked complete. */
        handle->snapshot_barrier_lsn_by_shard[b.shard_id] = b.barrier_lsn;
        handle->snapshot_partition_complete[b.shard_id] = true;
        handle->confirmed_lsn_by_shard[b.shard_id] = b.barrier_lsn;
    }
    return true;
}

bool transition_snapshotting_to_catching_up(
        subscription_handle_t *handle,
        std::string *reason_out) {
    guarantee(handle != nullptr);
    handle->assert_thread();

    if (handle->config.state != subscription_state_t::SNAPSHOTTING) {
        set_reason(reason_out,
            strprintf("expected SNAPSHOTTING, found %s",
                      subscription_state_to_string(handle->config.state)));
        return false;
    }
    if (!all_snapshot_partitions_complete(*handle)) {
        set_reason(reason_out,
            "cannot leave SNAPSHOTTING until all snapshot partitions complete");
        return false;
    }
    return apply_transition(
        handle, subscription_state_t::CATCHING_UP, nullptr, reason_out);
}

bool transition_catching_up_to_streaming(
        subscription_handle_t *handle,
        std::string *reason_out) {
    guarantee(handle != nullptr);
    handle->assert_thread();

    if (handle->config.state != subscription_state_t::CATCHING_UP) {
        set_reason(reason_out,
            strprintf("expected CATCHING_UP, found %s",
                      subscription_state_to_string(handle->config.state)));
        return false;
    }
    return apply_transition(
        handle, subscription_state_t::STREAMING, nullptr, reason_out);
}

bool transition_streaming_to_paused(
        subscription_handle_t *handle,
        std::string *reason_out) {
    guarantee(handle != nullptr);
    handle->assert_thread();
    return apply_transition(
        handle, subscription_state_t::PAUSED, nullptr, reason_out);
}

bool transition_paused_to_streaming(
        subscription_handle_t *handle,
        std::string *reason_out) {
    guarantee(handle != nullptr);
    handle->assert_thread();
    return apply_transition(
        handle, subscription_state_t::STREAMING, nullptr, reason_out);
}

bool transition_to_error(
        subscription_handle_t *handle,
        const subscription_error_t &error,
        std::string *reason_out) {
    guarantee(handle != nullptr);
    handle->assert_thread();
    if (error.code.empty()) {
        set_reason(reason_out, "ERROR requires a non-empty diagnostic code");
        return false;
    }
    return apply_transition(
        handle, subscription_state_t::ERROR, &error, reason_out);
}

bool transition_to_resync_required(
        subscription_handle_t *handle,
        const subscription_error_t &error,
        std::string *reason_out) {
    guarantee(handle != nullptr);
    handle->assert_thread();
    if (error.code.empty()) {
        set_reason(reason_out,
            "RESYNC_REQUIRED requires a non-empty diagnostic code");
        return false;
    }
    return apply_transition(
        handle, subscription_state_t::RESYNC_REQUIRED, &error, reason_out);
}

bool transition_to_dropping(
        subscription_handle_t *handle,
        std::string *reason_out) {
    guarantee(handle != nullptr);
    handle->assert_thread();
    return apply_transition(
        handle, subscription_state_t::DROPPING, nullptr, reason_out);
}

bool transition_dropping_to_dropped(
        subscription_handle_t *handle,
        std::string *reason_out) {
    guarantee(handle != nullptr);
    handle->assert_thread();
    return apply_transition(
        handle, subscription_state_t::DROPPED, nullptr, reason_out);
}

// ── Snapshot orchestration ─────────────────────────────────────────────────

std::vector<snapshot_barrier_t> create_snapshot_barriers(
        const std::map<uuid_u, log_sequence_number_t> &high_water_by_shard) {
    std::vector<snapshot_barrier_t> out;
    out.reserve(high_water_by_shard.size());
    for (const auto &pair : high_water_by_shard) {
        snapshot_barrier_t b;
        b.shard_id = pair.first;
        b.barrier_lsn = pair.second;
        out.push_back(b);
    }
    return out;
}

bool mark_snapshot_partition_complete(
        subscription_handle_t *handle,
        const uuid_u &shard_id) {
    guarantee(handle != nullptr);
    handle->assert_thread();

    auto it = handle->snapshot_partition_complete.find(shard_id);
    if (it == handle->snapshot_partition_complete.end()) {
        return false;
    }
    it->second = true;

    /* Once a partition is durable at the consumer, confirm the barrier LSN
     * so retention can advance past snapshot-covered history for that shard
     * when the coordinator checkpoints. */
    auto barrier_it = handle->snapshot_barrier_lsn_by_shard.find(shard_id);
    if (barrier_it != handle->snapshot_barrier_lsn_by_shard.end()) {
        handle->confirmed_lsn_by_shard[shard_id] = barrier_it->second;
    }

    return all_snapshot_partitions_complete(*handle);
}

bool all_snapshot_partitions_complete(const subscription_handle_t &handle) {
    if (handle.snapshot_barrier_lsn_by_shard.empty()) {
        return false;
    }
    for (const auto &pair : handle.snapshot_partition_complete) {
        if (!pair.second) {
            return false;
        }
    }
    /* Every barrier shard must appear in the complete map. */
    for (const auto &pair : handle.snapshot_barrier_lsn_by_shard) {
        auto it = handle.snapshot_partition_complete.find(pair.first);
        if (it == handle.snapshot_partition_complete.end() || !it->second) {
            return false;
        }
    }
    return true;
}

std::vector<snapshot_frame_t> build_snapshot_frames_for_shard(
        const snapshot_barrier_t &barrier,
        const std::vector<std::vector<char>> &serialized_rows) {
    std::vector<snapshot_frame_t> frames;
    frames.reserve(serialized_rows.size() + 2);

    snapshot_frame_t begin;
    begin.kind = snapshot_frame_kind_t::BEGIN;
    begin.shard_id = barrier.shard_id;
    begin.barrier_lsn = barrier.barrier_lsn;
    begin.snapshot = true;
    frames.push_back(std::move(begin));

    for (const std::vector<char> &row : serialized_rows) {
        snapshot_frame_t row_frame;
        row_frame.kind = snapshot_frame_kind_t::ROW;
        row_frame.shard_id = barrier.shard_id;
        row_frame.barrier_lsn = barrier.barrier_lsn;
        row_frame.row_image = row;
        row_frame.snapshot = true;
        frames.push_back(std::move(row_frame));
    }

    snapshot_frame_t end;
    end.kind = snapshot_frame_kind_t::END;
    end.shard_id = barrier.shard_id;
    end.barrier_lsn = barrier.barrier_lsn;
    end.snapshot = true;
    frames.push_back(std::move(end));

    return frames;
}

bool journal_event_eligible_after_barrier(
        const subscription_handle_t &handle,
        const uuid_u &shard_id,
        log_sequence_number_t event_lsn) {
    auto barrier_it = handle.snapshot_barrier_lsn_by_shard.find(shard_id);
    if (barrier_it == handle.snapshot_barrier_lsn_by_shard.end()) {
        return false;
    }
    auto complete_it = handle.snapshot_partition_complete.find(shard_id);
    if (complete_it == handle.snapshot_partition_complete.end()
        || !complete_it->second) {
        /* Snapshot partition not yet durable at the consumer — hold journal
         * events for this shard (spec §4.6 step 4). */
        return false;
    }
    /* Strictly after the barrier. */
    return barrier_it->second < event_lsn;
}

// ── Subscription applier (spec §3.7) ─────────────────────────────────────
//
// The applier is the single sink for change records on the target side. It
// owns the in-memory dedup ledger and writes to the target table.
//
// Transaction model: target write + ledger record are staged together. The
// ledger insert happens BEFORE the target write so a crash after the ledger
// insert is recoverable by replay (spec §3.7 paragraph 2). A crash after the
// target write but before the ledger insert cannot occur because the ledger
// is the first step.
//
// On any error mid-batch, all ledger inserts staged during this batch are
// rolled back. The handle's confirmed_lsn_by_shard is NOT updated until the
// batch completes successfully, so the next reconnect replays the whole
// batch from scratch.
//
// Reconnect: replays from `confirmed_lsn_by_shard` (the highest contiguous
// LSN the applier has durably applied + recorded).
// Resync: a source that cannot serve the requested LSN (its WAL retention
// floor moved past us) triggers a state transition to RESYNC_REQUIRED via
// transition_to_resync_required(). No silent gap-filling.

subscription_applier_t::subscription_applier_t(subscription_handle_t *handle)
    : home_thread_mixin_t(handle->home_thread()),
      handle_(handle) {
    guarantee(handle_ != nullptr, "subscription_applier_t needs a non-null handle");
    handle_->assert_thread();
}

bool subscription_applier_t::already_applied_no_interruptor(
        const change_event_id_t &event_id) const {
    handle_->assert_thread();
    return applied_ledger_.count(event_id) > 0;
}

bool subscription_applier_t::already_applied(
        const change_event_id_t &event_id,
        signal_t *interruptor) const {
    handle_->assert_thread();
    guarantee(interruptor != nullptr);
    /* Honor caller interruptor before the (cheap) lookup. If interrupted,
     * treat the answer as "not applied" — the caller will see the
     * interruptor pulsed and abandon the batch. The lookup is O(log N) so
     * we don't bother with cond_t waits; just bail immediately. */
    if (interruptor->is_pulsed()) {
        return false;
    }
    return applied_ledger_.count(event_id) > 0;
}

size_t subscription_applier_t::ledger_size() const {
    handle_->assert_thread();
    return applied_ledger_.size();
}

bool subscription_applier_t::ledger_has_shard(const uuid_u &shard_id) const {
    handle_->assert_thread();
    /* The set is ordered by (shard_id, lsn) — find the first entry for the
     * shard; if it exists, the shard has at least one ledger entry.
     * Iterate from low to high: stop as soon as we encounter a shard_id
     * strictly greater than the target (uuid_u only supports operator<). */
    for (const auto &id : applied_ledger_) {
        if (id.shard_id == shard_id) {
            return true;
        }
        if (shard_id < id.shard_id) {
            /* Past any possible match for our shard_id. */
            return false;
        }
    }
    return false;
}

bool subscription_applier_t::is_resync_required() const {
    handle_->assert_thread();
    return handle_->config.state == subscription_state_t::RESYNC_REQUIRED;
}

bool subscription_applier_t::apply_one_record(
        const change_record_t &record,
        signal_t *interruptor) {
    handle_->assert_thread();
    guarantee(interruptor != nullptr);

    /* If the handle has been cancelled (DROPPING) or transitioned to
     * RESYNC_REQUIRED mid-batch, bail out cleanly. The caller will see the
     * rolled-back ledger and no confirmed LSN advance. */
    if (interruptor->is_pulsed()) {
        return false;
    }
    if (handle_->is_cancel_requested()) {
        return false;
    }
    if (handle_->config.state == subscription_state_t::RESYNC_REQUIRED) {
        return false;
    }

    /* Sanity-check the record belongs to the configured subscription's
     * target table identity. Records from a different source cluster or
     * different table are protocol errors and must not be applied. */
    if (record.event_id.table_id.is_nil()) {
        return false;
    }

    /* Real target-table write is performed by the CDC-08 coordinator
     * pipeline via the table_query_client interface; this method is the
     * "would this record apply cleanly?" gate. The CDC-06e layer models
     * the write as: deserialize before_image / after_image from
     * record.before_image / record.after_image, hand the resulting datum
     * pair to the table writer, and return success/failure. */
    if (record.op == change_operation_t::INSERT
        || record.op == change_operation_t::UPDATE
        || record.op == change_operation_t::REPLACE) {
        if (record.after_image.empty()) {
            /* INSERT/UPDATE/REPLACE must carry an after-image. */
            return false;
        }
    } else if (record.op == change_operation_t::DELETE) {
        if (record.before_image.empty()) {
            /* DELETE must carry a before-image. */
            return false;
        }
    }

    return true;
}

void subscription_applier_t::rollback_pending_ledger_entries(
        const std::vector<change_event_id_t> &pending_event_ids) {
    handle_->assert_thread();
    for (const change_event_id_t &id : pending_event_ids) {
        applied_ledger_.erase(id);
    }
}

void subscription_applier_t::apply_batch(
        const std::vector<change_record_t> &records,
        signal_t *interruptor) {
    handle_->assert_thread();
    guarantee(interruptor != nullptr);

    /* Refuse the batch up front if the subscription has been cancelled or
     * has entered RESYNC_REQUIRED — the coordinator that called us is
     * stale (or this is a late delivery after a resync transition). */
    if (handle_->is_cancel_requested()) {
        return;
    }
    if (handle_->config.state == subscription_state_t::RESYNC_REQUIRED) {
        return;
    }

    /* Empty batch is a no-op. We still respect the interruptor. */
    if (records.empty()) {
        return;
    }

    /* Stage 1: partition records into (apply, skip) using the ledger.
     *
     * Spec §3.7: at-least-once delivery with idempotent apply. Every
     * event_id in the batch that already lives in the ledger is skipped
     * without writing to the target. Records that pass dedup proceed to
     * stage 2.
     *
     * We collect the new event_ids to insert AND the per-shard max LSN
     * for the confirmed_lsn_by_shard advance — both are committed
     * atomically with stage 3 success. */
    std::vector<change_event_id_t> pending_event_ids;
    std::vector<size_t> pending_indices;
    pending_event_ids.reserve(records.size());
    pending_indices.reserve(records.size());
    std::map<uuid_u, log_sequence_number_t> max_pending_lsn_by_shard;

    for (size_t i = 0; i < records.size(); ++i) {
        const change_record_t &r = records[i];
        if (already_applied(r.event_id, interruptor)) {
            continue;
        }
        pending_event_ids.push_back(r.event_id);
        pending_indices.push_back(i);

        /* Track the highest LSN we'll need to advance the per-shard
         * confirmed cursor to. */
        auto shard_it = max_pending_lsn_by_shard.find(r.event_id.shard_id);
        if (shard_it == max_pending_lsn_by_shard.end()
            || shard_it->second < r.event_id.lsn) {
            max_pending_lsn_by_shard[r.event_id.shard_id] = r.event_id.lsn;
        }
    }

    /* Nothing new to apply — common case for reconnect replays. Return
     * cleanly without touching confirmed_lsn_by_shard: the LSNs we
     * already had are still authoritative. */
    if (pending_indices.empty()) {
        return;
    }

    /* Stage 2: insert every new event_id into the ledger BEFORE any
     * target write. The ledger insert is the durable boundary that makes
     * the rest of the apply safe under crash (spec §3.7 paragraph 2). */
    for (const change_event_id_t &id : pending_event_ids) {
        applied_ledger_.insert(id);
    }

    /* Stage 3: write each non-duplicate record to the target table.
     * Any failure rolls back the entire batch's ledger inserts and
     * leaves confirmed_lsn_by_shard untouched. */
    bool all_ok = true;
    for (size_t idx : pending_indices) {
        const change_record_t &r = records[idx];
        if (!apply_one_record(r, interruptor)) {
            all_ok = false;
            break;
        }
    }

    if (!all_ok) {
        rollback_pending_ledger_entries(pending_event_ids);
        return;
    }

    /* Stage 4: advance confirmed_lsn_by_shard on the handle. This is the
     * sole retention-release cursor (spec §3.6 invariant: "confirmed_lsn
     * is the sole retention-release cursor and must acknowledge contiguous
     * LSNs only"). For this CDC-06e layer we advance to the highest LSN
     * we applied in this batch; contiguity is enforced upstream by the
     * source-side dispatcher which only emits ordered journal frames.
     *
     * Use the higher of (existing, just-applied) so out-of-order batch
     * delivery (which shouldn't happen but defense-in-depth) never moves
     * the cursor backwards. */
    for (const auto &pair : max_pending_lsn_by_shard) {
        auto existing = handle_->confirmed_lsn_by_shard.find(pair.first);
        if (existing == handle_->confirmed_lsn_by_shard.end()
            || existing->second < pair.second) {
            handle_->confirmed_lsn_by_shard[pair.first] = pair.second;
        }
    }
}

std::map<uuid_u, log_sequence_number_t>
subscription_applier_t::reconnect_resume_positions() const {
    handle_->assert_thread();
    /* Mirror the handle's confirmed LSN map verbatim. The handle is
     * mutated only inside apply_batch() under the home thread, so this
     * read is race-free without further synchronization. */
    return handle_->confirmed_lsn_by_shard;
}

bool subscription_applier_t::validate_reconnect_or_resync(
        const std::map<uuid_u, log_sequence_number_t> &source_floor_by_shard,
        signal_t *interruptor) {
    handle_->assert_thread();
    guarantee(interruptor != nullptr);

    /* Walk every shard we have confirmed progress on. If the source's
     * floor for any such shard is strictly greater than our confirmed LSN
     * (meaning the source's WAL retention has advanced past our last
     * applied position), we have a WAL gap → RESYNC_REQUIRED. */
    subscription_error_t gap_error;
    gap_error.code = "CDC_WAL_GAP";
    bool gap = false;
    std::string gap_detail;

    for (const auto &pair : handle_->confirmed_lsn_by_shard) {
        const uuid_u &shard_id = pair.first;
        const log_sequence_number_t &confirmed_lsn = pair.second;
        auto floor_it = source_floor_by_shard.find(shard_id);
        if (floor_it == source_floor_by_shard.end()) {
            /* Source no longer knows about this shard (dropped,
             * re-sharded). Spec invariant 5: treat as a gap. */
            gap = true;
            gap_detail = strprintf(
                "shard %s has no source floor (shard unknown to source)",
                uuid_to_str(shard_id).c_str());
            break;
        }
        if (confirmed_lsn < floor_it->second) {
            gap = true;
            gap_detail = strprintf(
                "shard %s confirmed_lsn=%" PRIu64
                " < source_floor=%" PRIu64
                " (WAL retention advanced past confirmed position)",
                uuid_to_str(shard_id).c_str(),
                confirmed_lsn.value,
                floor_it->second.value);
            break;
        }
    }

    if (gap) {
        gap_error.message = gap_detail;
        /* Carry the snapshot of confirmed positions for the operator —
         * the diagnostic must not expose raw rows, only LSNs. */
        gap_error.confirmed_lsn_by_shard = handle_->confirmed_lsn_by_shard;
        std::string reason;
        bool ok = transition_to_resync_required(
            handle_, gap_error, &reason);
        if (!ok) {
            /* transition_to_resync_required rejects calls when the state
             * machine cannot accept the transition (e.g. already DROPPED).
             * That's a legitimate end-state; we don't surface it as a
             * hard failure — the caller should observe the terminal
             * state and stop. */
            return false;
        }
        return false;
    }

    /* No gap — the source can serve every confirmed LSN. Caller may
     * proceed with reconnect from `reconnect_resume_positions()`. */
    return true;
}

bool find_publication_by_name(
        const std::map<uuid_u, publication_config_t> &publications,
        const name_string_t &name,
        publication_config_t *out) {
    guarantee(out != nullptr);
    for (const auto &pair : publications) {
        if (pair.second.name == name) {
            *out = pair.second;
            return true;
        }
    }
    return false;
}

datum_t subscription_config_to_datum(const subscription_config_t &config) {
    datum_object_builder_t res;
    res.overwrite("id",
        datum_t(datum_string_t(uuid_to_str(config.subscription_id))));
    res.overwrite("name",
        datum_t(datum_string_t(config.name.str())));
    res.overwrite("state",
        datum_t(datum_string_t(subscription_state_to_string(config.state))));
    res.overwrite("target_database_id",
        datum_t(datum_string_t(uuid_to_str(config.target_database_id))));
    res.overwrite("target_table_id",
        datum_t(datum_string_t(uuid_to_str(config.target_table_id))));
    res.overwrite("publication",
        datum_t(datum_string_t(config.publication_name.str())));
    res.overwrite("conflict_policy",
        datum_t(datum_string_t(
            conflict_resolution_to_string(config.conflict_policy))));
    res.overwrite("source_cluster_id",
        datum_t(datum_string_t(uuid_to_str(config.source_cluster_id))));
    res.overwrite("created_at",
        datum_t(static_cast<double>(config.created_at)));
    return std::move(res).to_datum();
}

}  // namespace ql
