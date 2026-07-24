// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_CONFLICT_RESOLVER_HPP_
#define RDB_PROTOCOL_CONFLICT_RESOLVER_HPP_

#include <functional>
#include <string>

#include "rdb_protocol/cdc_types.hpp"

namespace ql {

enum class conflict_resolution_policy_t {
    LAST_WRITE_WINS,       // default
    SOURCE_WINS,
    TARGET_WINS,
    CUSTOM_HANDLER,        // ReQL function
    MANUAL                 // operator intervention
};

struct conflict_resolution_result_t {
    change_record_t resolved;
    bool skipped = false;
    std::string reason;
};

class conflict_resolver_t {
public:
    explicit conflict_resolver_t(conflict_resolution_policy_t policy);

    // Resolve a target change against a source change.
    conflict_resolution_result_t resolve(
        const change_record_t &source,
        const change_record_t &target) const;

    // Register a custom ReQL handler (for CUSTOM_HANDLER policy).
    void set_custom_handler(std::function<conflict_resolution_result_t(
        const change_record_t&, const change_record_t&)> handler);

    conflict_resolution_policy_t policy() const;

private:
    conflict_resolution_policy_t policy_;
    std::function<conflict_resolution_result_t(
        const change_record_t&, const change_record_t&)> custom_handler_;

    // LWW: compare (commit_timestamp, source_cluster_id, event_id.shard_id, event_id.lsn)
    static bool is_newer(const change_record_t &a, const change_record_t &b);
};

}  // namespace ql

#endif  // RDB_PROTOCOL_CONFLICT_RESOLVER_HPP_
