// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/conflict_resolver.hpp"

#include "rdb_protocol/datum.hpp"

namespace ql {

conflict_resolver_t::conflict_resolver_t(conflict_resolution_policy_t policy)
    : policy_(policy) {
}

conflict_resolution_policy_t conflict_resolver_t::policy() const {
    return policy_;
}

void conflict_resolver_t::set_custom_handler(
        std::function<conflict_resolution_result_t(
            const change_record_t&, const change_record_t&)> handler) {
    custom_handler_ = std::move(handler);
}

// is_newer: tuple comparison:
//   1. commit_timestamp (newer wins)
//   2. source_cluster_id string (tiebreak)
//   3. shard_id string (tiebreak)
//   4. LSN value (tiebreak)
bool conflict_resolver_t::is_newer(const change_record_t &a,
                                   const change_record_t &b) {
    if (a.commit_timestamp != b.commit_timestamp) {
        return a.commit_timestamp > b.commit_timestamp;
    }
    // Tiebreak by cluster_id
    if (a.event_id.source_cluster_id != b.event_id.source_cluster_id) {
        return !(a.event_id.source_cluster_id < b.event_id.source_cluster_id);
    }
    // Tiebreak by shard_id
    if (a.event_id.shard_id != b.event_id.shard_id) {
        return !(a.event_id.shard_id < b.event_id.shard_id);
    }
    // Tiebreak by LSN
    return a.event_id.lsn.value > b.event_id.lsn.value;
}

conflict_resolution_result_t conflict_resolver_t::resolve(
        const change_record_t &source,
        const change_record_t &target) const {
    conflict_resolution_result_t result;

    // DELETE vs DELETE: skip (both deleted)
    if (source.op == change_operation_t::DELETE
        && target.op == change_operation_t::DELETE) {
        result.skipped = true;
        result.reason = "both records are DELETE";
        return result;
    }

    // INSERT vs INSERT: source wins (source is the canonical stream)
    if (source.op == change_operation_t::INSERT
        && target.op == change_operation_t::INSERT) {
        result.resolved = source;
        result.skipped = false;
        return result;
    }

    switch (policy_) {
    case conflict_resolution_policy_t::LAST_WRITE_WINS: {
        if (is_newer(source, target)) {
            result.resolved = source;
        } else {
            // same age or target is newer: source wins when same age
            result.resolved = target;
        }
        break;
    }
    case conflict_resolution_policy_t::SOURCE_WINS: {
        result.resolved = source;
        break;
    }
    case conflict_resolution_policy_t::TARGET_WINS: {
        result.resolved = target;
        break;
    }
    case conflict_resolution_policy_t::CUSTOM_HANDLER: {
        if (custom_handler_) {
            return custom_handler_(source, target);
        }
        // Fall through to LWW if no handler is set
        if (is_newer(source, target)) {
            result.resolved = source;
        } else {
            result.resolved = target;
        }
        break;
    }
    case conflict_resolution_policy_t::MANUAL: {
        result.skipped = true;
        result.reason = "manual intervention required";
        break;
    }
    }

    return result;
}

datum_t conflict_resolver_t::apply_merge(
        const change_record_t &resolved,
        const datum_t &target_current,
        bool target_exists) const {
    // DELETE: return null datum (caller drops the row)
    if (resolved.op == change_operation_t::DELETE) {
        return datum_t();
    }

    // Tombstone (_cdc_tombstone in before_image): treat as DELETE
    if (!resolved.before_image.empty()) {
        datum_t before = deserialize_datum_from_vector(resolved.before_image);
        if (before.has() && before.get_type() == datum_t::R_OBJECT) {
            datum_t tombstone = before.get_field("_cdc_tombstone", NOTHROW);
            if (tombstone.has()) {
                return datum_t();
            }
        }
    }

    datum_t after = deserialize_datum_from_vector(resolved.after_image);

    switch (resolved.op) {
    case change_operation_t::INSERT:
        if (target_exists) {
            // Merge: take source's new fields, keep target's existing
            // fields that source doesn't touch
            return target_current.merge(after);
        }
        return after;

    case change_operation_t::UPDATE:
        if (target_exists) {
            // Deep merge: after_image overwrites matching keys in
            // target_current; target-only keys survive
            return target_current.merge(after);
        }
        // Target doesn't exist: treat as INSERT
        return after;

    case change_operation_t::REPLACE:
        // Full replacement unconditionally
        return after;

    case change_operation_t::DELETE:
        // Already handled above, but keep for completeness
        return datum_t();
    }

    return datum_t();
}

bool conflict_resolver_t::detect_conflict(
        const change_record_t &source,
        const change_record_t &target) const {
    // Different table → different PK, no conflict
    if (source.event_id.table_id != target.event_id.table_id) {
        return false;
    }

    // Determine row identity: for INSERT use after_image,
    // for UPDATE/DELETE/REPLACE use before_image
    const std::vector<char> &source_row_id = (source.op == change_operation_t::INSERT)
        ? source.after_image : source.before_image;
    const std::vector<char> &target_row_id = (target.op == change_operation_t::INSERT)
        ? target.after_image : target.before_image;

    // Different PK → no conflict
    if (source_row_id != target_row_id) {
        return false;
    }

    // Same PK, same op, same after_image → no conflict (idempotent)
    if (source.op == target.op
            && source.after_image == target.after_image) {
        return false;
    }

    // Same PK + different op → conflict
    // Same PK + same op + different after_image → conflict
    return true;
}

}  // namespace ql
