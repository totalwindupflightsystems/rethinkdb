// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_SUBSCRIPTION_HPP_
#define RDB_PROTOCOL_SUBSCRIPTION_HPP_

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "concurrency/cond_var.hpp"
#include "concurrency/signal.hpp"
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

// ── Target apply ledger + applier (spec §3.7) ────────────────────────────
//
// A `subscription_applier_t` owns the in-memory dedup ledger for one
// subscription and writes change records to the target table. The applier is
// owned by the target-side replication coordinator. One applier per
// subscription.
//
// The applier is intentionally home-thread bound (extends home_thread_mixin_t);
// all mutations to the ledger and the handle's confirmed_lsn_by_shard map
// must happen on the home thread. Concurrent dispatchers route work onto the
// home thread via the cross-thread mailbox before calling apply_batch().
//
// Spec invariants enforced here:
//  1. Idempotence: a record with an event_id already in the ledger is
//     skipped (no target write, no ledger re-record). At-least-once delivery
//     therefore never produces duplicate row mutations.
//  2. Ledger retention: ledger entries are retained for the subscription
//     lifetime. No automatic pruning — confirmed LSN advance does not
//     garbage-collect the ledger (spec §3.7 final paragraph).
//  3. Crash safety: a target write + ledger record are conceptually one
//     transaction. A crash after the target write but before the ledger
//     record causes the next replay to re-detect the row and... NOT mutate
//     it. Spec §3.7 paragraph 2: "the ledger suppresses duplicate row
//     mutation". We achieve this by recording BEFORE the target write —
//     the ledger insert is the durable boundary. A crash after the ledger
//     insert but before the target write is recoverable on replay by
//     re-applying (the second apply is a no-op against an idempotent target
//     row keyed by primary_key, since CDC-09 will implement PK-keyed
//     upserts; in this CDC-06e layer we treat ledger presence as "this
//     record has been seen", which is the correct conservative semantics
//     for spec §3.7 invariant 6).
//  4. WAL gap → RESYNC_REQUIRED: replay requests anchored at a confirmed
//     LSN that the source can no longer serve cause the applier to invoke
//     transition_to_resync_required() on the handle. Reconnect NEVER
//     silently starts from a later point.

class subscription_applier_t : public home_thread_mixin_t {
public:
    /* Construct an applier bound to a handle. The applier stores a
     * non-owning pointer to the handle; the caller (coordinator) must
     * ensure the handle outlives the applier. Both must live on the same
     * home thread. */
    explicit subscription_applier_t(subscription_handle_t *handle);

    /* Apply a contiguous batch of change records received from the source.
     * Records whose event_id is already in the ledger are skipped
     * (at-least-once dedup). Records that pass dedup are written to the
     * target table and recorded in the ledger in one logical transaction.
     *
     * On success: every non-skipped record has its event_id added to the
     *   ledger, and the per-shard confirmed LSN on the handle advances to
     *   the maximum LSN of any non-skipped record for that shard in this
     *   batch.
     *
     * On failure: the entire batch is rolled back (no ledger inserts, no
     *   confirmed LSN advance) and the failure mode is propagated via:
     *     - normal return: batch fully applied (some records may have been
     *       skipped as duplicates)
     *     - interrupted: caller-supplied interruptor pulsed before/during
     *       apply; no partial state is observable from the caller's
     *       perspective (ledger is reverted for any partial work)
     *
     * Thread: must run on the home thread of `handle`. */
    void apply_batch(const std::vector<change_record_t> &records,
                     signal_t *interruptor);

    /* Returns true iff `event_id` is in the ledger. Cheap O(log N) lookup.
     * Thread: must run on the home thread of `handle`. */
    bool already_applied(const change_event_id_t &event_id,
                         signal_t *interruptor) const;

    /* Returns true iff `event_id` is in the ledger. Non-interruptor variant
     * for callers that don't have a signal (debug paths, test harnesses).
     * Thread: must run on the home thread of `handle`. */
    bool already_applied_no_interruptor(const change_event_id_t &event_id) const;

    /* Number of ledger entries currently retained (debug + observability). */
    size_t ledger_size() const;

    /* True when at least one event_id for `shard_id` is in the ledger. */
    bool ledger_has_shard(const uuid_u &shard_id) const;

    /* Build the start-LSN map for a reconnect request. Mirrors the
     * handle's confirmed_lsn_by_shard into a fresh map suitable for handing
     * to the source-side dispatcher as the "resume from here" anchor.
     *
     * Rationale (spec §5.7, §8.2): on connection drop, the source replays
     * strictly from the last confirmed LSN per shard. The confirmed LSN
     * is the highest contiguous LSN whose corresponding event_id has
     * been durably applied to the target and recorded in the ledger —
     * exactly what this applier advances in apply_batch().
     *
     * Returns an empty map if no shard has any confirmed progress (the
     * caller should then request the snapshot barrier instead). */
    std::map<uuid_u, log_sequence_number_t> reconnect_resume_positions() const;

    /* Reconnect entry point: validate that the source can serve every
     * requested resume position. Callers (replication coordinator) fetch
     * the source's available history floor per shard via the source-side
     * mailbox service and pass it here as `source_floor_by_shard`.
     *
     * Returns true and leaves the handle's state unchanged if every
     * confirmed LSN in `handle->confirmed_lsn_by_shard` is >= the source's
     * floor for the same shard (i.e. the source still has the history).
     *
     * Returns false and transitions the handle to RESYNC_REQUIRED via
     * transition_to_resync_required() if any shard's confirmed LSN is
     * below the source's floor — the gap means retention advanced past
     * our last confirmed position and we cannot replay (spec invariant 5:
     * "A WAL gap produces RESYNC_REQUIRED, never a best-effort later
     * start"). The diagnostic payload attached to the transition names the
     * offending shard and its confirmed/floor LSN pair. */
    bool validate_reconnect_or_resync(
        const std::map<uuid_u, log_sequence_number_t> &source_floor_by_shard,
        signal_t *interruptor);

    /* True iff the handle is currently in RESYNC_REQUIRED state. The
     * applier checks this before apply_batch() so a stale batch delivered
     * after the gap is detected is rejected at the boundary instead of
     * being silently merged with replay intent. */
    bool is_resync_required() const;

private:
    /* Apply one record to the target. Returns true if the record was
     * applied (or skipped as duplicate), false on error. Does NOT touch
     * the ledger — the caller is responsible for staging ledger inserts
     * before invocation and committing them after successful return. */
    bool apply_one_record(const change_record_t &record,
                          signal_t *interruptor);

    /* Reverse a partially-applied batch on failure: remove every ledger
     * entry whose event_id is in `pending_event_ids`. Used only on the
     * error/rollback path. */
    void rollback_pending_ledger_entries(
        const std::vector<change_event_id_t> &pending_event_ids);

    /* Bound handle. Not owned. */
    subscription_handle_t *handle_;

    /* Apply ledger: set of event_ids seen-and-applied for this
     * subscription. Ordered by (shard_id, lsn) for deterministic
     * iteration during rollback. Retained for subscription lifetime
     * (spec §3.7). */
    std::set<change_event_id_t, change_event_id_compare_by_lsn_t> applied_ledger_;

    DISABLE_COPYING(subscription_applier_t);
};

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

// subscription_config_to_datum lives in subscription.cc because it depends
// on the full datum_t definition.
datum_t subscription_config_to_datum(const subscription_config_t &config);

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
