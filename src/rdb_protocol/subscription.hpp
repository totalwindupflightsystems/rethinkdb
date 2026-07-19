// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_SUBSCRIPTION_HPP_
#define RDB_PROTOCOL_SUBSCRIPTION_HPP_

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "concurrency/cond_var.hpp"
#include "containers/name_string.hpp"
#include "containers/optional.hpp"
#include "containers/uuid.hpp"
#include "rdb_protocol/cdc_types.hpp"
#include "rdb_protocol/publication.hpp"
#include "rpc/serialize_macros.hpp"
#include "threading.hpp"
#include "time.hpp"

namespace ql {

class datum_t;

// ── Subscription lifecycle (spec §3.3) ──
//
// CREATING → CONNECTING → SNAPSHOTTING → CATCHING_UP → STREAMING (happy path)
// Any non-terminal state → DROPPING → DROPPED (terminal)
// Any non-terminal state → ERROR (operator retry returns to CREATING)
// Any non-terminal state → RESYNC_REQUIRED (WAL gap, history loss)
// STREAMING ↔ PAUSED (pause retains position)
//
// Numeric values for the original 0..7 set are FROZEN so existing on-disk
// / unit-test ordinals stay stable. PAUSED and RESYNC_REQUIRED are added at
// the end of the range by CDC-06c.
enum class subscription_state_t {
    CREATING = 0,
    CONNECTING = 1,
    SNAPSHOTTING = 2,
    CATCHING_UP = 3,
    STREAMING = 4,
    DROPPING = 5,
    DROPPED = 6,
    ERROR = 7,
    PAUSED = 8,
    RESYNC_REQUIRED = 9
};

enum class conflict_resolution_t {
    LAST_WRITE_WINS = 0, PRIMARY_KEY_MERGE = 1, CUSTOM_HANDLER = 2
};

struct subscription_config_t {
    uuid_u subscription_id;
    name_string_t name;
    uuid_u target_database_id;
    uuid_u target_table_id;
    name_string_t publication_name;
    uuid_u source_cluster_id;
    conflict_resolution_t conflict_policy = conflict_resolution_t::LAST_WRITE_WINS;
    subscription_state_t state = subscription_state_t::CREATING;
    uuid_u created_by_user_id;
    microtime_t created_at;

    bool operator==(const subscription_config_t &other) const {
        return subscription_id == other.subscription_id
            && name == other.name
            && target_database_id == other.target_database_id
            && target_table_id == other.target_table_id
            && publication_name == other.publication_name
            && source_cluster_id == other.source_cluster_id
            && conflict_policy == other.conflict_policy
            && state == other.state
            && created_by_user_id == other.created_by_user_id
            && created_at == other.created_at;
    }
    bool operator!=(const subscription_config_t &other) const {
        return !(*this == other);
    }
};

// Diagnostic payload attached when a subscription enters ERROR or
// RESYNC_REQUIRED. Never carries credentials or raw document bodies.
struct subscription_error_t {
    std::string code;
    std::string message;
    microtime_t occurred_at = 0;
    /* Highest contiguous confirmed LSN per shard at the time of failure
     * (empty if none were confirmed yet). */
    std::map<uuid_u, log_sequence_number_t> confirmed_lsn_by_shard;

    bool operator==(const subscription_error_t &other) const {
        return code == other.code
            && message == other.message
            && occurred_at == other.occurred_at
            && confirmed_lsn_by_shard == other.confirmed_lsn_by_shard;
    }
    bool operator!=(const subscription_error_t &other) const {
        return !(*this == other);
    }
};

// Spec §4.6 — one per-shard snapshot barrier LSN. The barrier is the LSN at
// which the consistent snapshot was taken: journal events with lsn <= barrier
// are covered by the snapshot rows; events with lsn > barrier stream after.
struct snapshot_barrier_t {
    uuid_u shard_id;
    log_sequence_number_t barrier_lsn;

    bool operator==(const snapshot_barrier_t &other) const {
        return shard_id == other.shard_id && barrier_lsn == other.barrier_lsn;
    }
    bool operator!=(const snapshot_barrier_t &other) const {
        return !(*this == other);
    }
};

// Snapshot frames are NOT synthetic insert events. They carry an explicit
// snapshot flag so target appliers bootstrap deliberately (spec §4.6).
enum class snapshot_frame_kind_t : uint8_t {
    BEGIN = 0,
    ROW = 1,
    END = 2
};

struct snapshot_frame_t {
    snapshot_frame_kind_t kind = snapshot_frame_kind_t::ROW;
    uuid_u shard_id;
    log_sequence_number_t barrier_lsn;
    /* Serialized row image (empty for BEGIN/END). */
    std::vector<char> row_image;
    /* Always true for snapshot frames — never implied by op type. */
    bool snapshot = true;
};

// Runtime handle for an in-flight subscription. Owned by the replication
// coordinator (or tests). Not Raft metadata — progress is checkpointed
// separately via the coordinator.
class subscription_handle_t : public home_thread_mixin_t {
public:
    subscription_handle_t();
    explicit subscription_handle_t(subscription_config_t config);

    subscription_config_t config;
    /* Resolved source publication identity (filled on CREATING→CONNECTING). */
    uuid_u publication_id;
    uuid_u source_table_id;
    uuid_u source_database_id;
    /* Replication slot bound to this subscription. */
    uuid_u slot_id;
    snapshot_mode_t snapshot_mode = snapshot_mode_t::FULL;

    std::map<uuid_u, log_sequence_number_t> snapshot_barrier_lsn_by_shard;
    std::map<uuid_u, log_sequence_number_t> confirmed_lsn_by_shard;
    /* Per-shard flag: snapshot partition rows have been delivered + ACK'd. */
    std::map<uuid_u, bool> snapshot_partition_complete;

    optional<subscription_error_t> last_error;

    /* Pulsed when entering DROPPING; in-flight ops must watch this (combined
     * with the caller's interruptor via wait_any_t). */
    cond_t *get_cancel_signal() {
        assert_thread();
        return &cancel_signal_;
    }
    const cond_t *get_cancel_signal() const {
        assert_thread();
        return &cancel_signal_;
    }
    bool is_cancel_requested() const {
        assert_thread();
        return cancel_signal_.is_pulsed();
    }

    /* Pulse the cancel signal. Idempotent. */
    void request_cancel();

private:
    cond_t cancel_signal_;
    DISABLE_COPYING(subscription_handle_t);
};

// ── State machine (spec §3.3) ────────────────────────────────────────────

/* Returns true if `from → to` is a permitted transition. On false, writes a
 * human-readable reason into `reason_out` when non-null. */
bool validate_state_transition(
    subscription_state_t from,
    subscription_state_t to,
    std::string *reason_out);

/* Apply a validated transition on `handle`. Updates `handle->config.state`.
 * For ERROR / RESYNC_REQUIRED, `error` must be non-null and is copied onto
 * the handle. For DROPPING, cancels in-flight ops via request_cancel().
 * Returns false (and leaves state unchanged) if the transition is illegal. */
bool apply_transition(
    subscription_handle_t *handle,
    subscription_state_t to,
    const subscription_error_t *error,
    std::string *reason_out);

/* Named transition helpers — thin wrappers that document the happy path and
 * call apply_transition with the right preconditions. */

/* CREATING → CONNECTING after Raft create commit + publication validation.
 * `publication_id` / source table ids are recorded on the handle. */
bool transition_creating_to_connecting(
    subscription_handle_t *handle,
    const uuid_u &publication_id,
    const uuid_u &source_database_id,
    const uuid_u &source_table_id,
    std::string *reason_out);

/* CONNECTING → SNAPSHOTTING when the consumer requests snapshot mode.
 * `barriers` must be non-empty for FULL snapshot. Records barriers on the handle. */
bool transition_connecting_to_snapshotting(
    subscription_handle_t *handle,
    const std::vector<snapshot_barrier_t> &barriers,
    const uuid_u &slot_id,
    std::string *reason_out);

/* CONNECTING → CATCHING_UP for snapshot_mode=NONE (no snapshot partition). */
bool transition_connecting_to_catching_up(
    subscription_handle_t *handle,
    const std::vector<snapshot_barrier_t> &live_start_positions,
    const uuid_u &slot_id,
    std::string *reason_out);

/* SNAPSHOTTING → CATCHING_UP after every snapshot partition is delivered. */
bool transition_snapshotting_to_catching_up(
    subscription_handle_t *handle,
    std::string *reason_out);

/* CATCHING_UP → STREAMING when the consumer has caught up to current LSN. */
bool transition_catching_up_to_streaming(
    subscription_handle_t *handle,
    std::string *reason_out);

/* STREAMING → PAUSED / PAUSED → STREAMING — pause retains position. */
bool transition_streaming_to_paused(
    subscription_handle_t *handle,
    std::string *reason_out);
bool transition_paused_to_streaming(
    subscription_handle_t *handle,
    std::string *reason_out);

/* Any non-terminal → ERROR. */
bool transition_to_error(
    subscription_handle_t *handle,
    const subscription_error_t &error,
    std::string *reason_out);

/* Any non-terminal → RESYNC_REQUIRED (WAL gap / history loss). */
bool transition_to_resync_required(
    subscription_handle_t *handle,
    const subscription_error_t &error,
    std::string *reason_out);

/* Any non-terminal → DROPPING (cancels in-flight), then DROPPING → DROPPED. */
bool transition_to_dropping(
    subscription_handle_t *handle,
    std::string *reason_out);
bool transition_dropping_to_dropped(
    subscription_handle_t *handle,
    std::string *reason_out);

// ── Snapshot orchestration (spec §4.6) ──────────────────────────────────

/* Build per-shard snapshot barriers from caller-supplied high-water LSNs.
 * `high_water_by_shard` maps shard_id → current durable high-water LSN; the
 * barrier for each shard is that value (snapshot covers ≤ barrier). */
std::vector<snapshot_barrier_t> create_snapshot_barriers(
    const std::map<uuid_u, log_sequence_number_t> &high_water_by_shard);

/* Mark one shard's snapshot partition complete. Returns true when ALL
 * partitions known on the handle are complete (ready for CATCHING_UP). */
bool mark_snapshot_partition_complete(
    subscription_handle_t *handle,
    const uuid_u &shard_id);

/* True when every shard in snapshot_barrier_lsn_by_shard has
 * snapshot_partition_complete[shard] == true. */
bool all_snapshot_partitions_complete(const subscription_handle_t &handle);

/* Build SNAPSHOT_BEGIN / SNAPSHOT_ROW / SNAPSHOT_END frames for one shard. */
std::vector<snapshot_frame_t> build_snapshot_frames_for_shard(
    const snapshot_barrier_t &barrier,
    const std::vector<std::vector<char>> &serialized_rows);

/* Journal events are eligible for streaming on a shard only after that
 * shard's snapshot partition is complete AND the event LSN is strictly
 * greater than the barrier. */
bool journal_event_eligible_after_barrier(
    const subscription_handle_t &handle,
    const uuid_u &shard_id,
    log_sequence_number_t event_lsn);

// ── Inline helpers (kept in the header for performance + cross-TU access) ─

inline const char *subscription_state_to_string(subscription_state_t state) {
    switch (state) {
    case subscription_state_t::CREATING:        return "creating";
    case subscription_state_t::CONNECTING:      return "connecting";
    case subscription_state_t::SNAPSHOTTING:    return "snapshotting";
    case subscription_state_t::CATCHING_UP:     return "catching_up";
    case subscription_state_t::STREAMING:       return "streaming";
    case subscription_state_t::DROPPING:        return "dropping";
    case subscription_state_t::DROPPED:         return "dropped";
    case subscription_state_t::ERROR:           return "error";
    case subscription_state_t::PAUSED:          return "paused";
    case subscription_state_t::RESYNC_REQUIRED: return "resync_required";
    default:                                    return "unknown";
    }
}

inline const char *conflict_resolution_to_string(conflict_resolution_t policy) {
    switch (policy) {
    case conflict_resolution_t::LAST_WRITE_WINS:   return "last_write_wins";
    case conflict_resolution_t::PRIMARY_KEY_MERGE: return "primary_key_merge";
    case conflict_resolution_t::CUSTOM_HANDLER:    return "custom_handler";
    default:                                       return "unknown";
    }
}

// Defined in terms/cdc_subscription.cc — avoids pulling datum headers
// into every compilation unit that includes this header.
datum_t subscription_config_to_datum(
        const subscription_config_t &config);

/* Look up a publication by name in a publications map. Returns true and
 * fills `out` on success. Used by CREATING→CONNECTING validation. */
bool find_publication_by_name(
    const std::map<uuid_u, publication_config_t> &publications,
    const name_string_t &name,
    publication_config_t *out);

}  // namespace ql

ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    ql::subscription_state_t, int8_t,
    ql::subscription_state_t::CREATING, ql::subscription_state_t::RESYNC_REQUIRED);
ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    ql::conflict_resolution_t, int8_t,
    ql::conflict_resolution_t::LAST_WRITE_WINS, ql::conflict_resolution_t::CUSTOM_HANDLER);
ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    ql::snapshot_frame_kind_t, int8_t,
    ql::snapshot_frame_kind_t::BEGIN, ql::snapshot_frame_kind_t::END);
RDB_DECLARE_SERIALIZABLE(ql::subscription_config_t);
RDB_DECLARE_SERIALIZABLE(ql::subscription_error_t);
RDB_DECLARE_SERIALIZABLE(ql::snapshot_barrier_t);
RDB_DECLARE_SERIALIZABLE(ql::snapshot_frame_t);

#endif  // RDB_PROTOCOL_SUBSCRIPTION_HPP_