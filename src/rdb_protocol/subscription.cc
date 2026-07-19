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
