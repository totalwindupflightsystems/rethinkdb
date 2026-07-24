// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/conflict_resolver.hpp"

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

}  // namespace ql
