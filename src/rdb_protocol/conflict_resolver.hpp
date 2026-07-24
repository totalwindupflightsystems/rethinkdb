// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_CONFLICT_RESOLVER_HPP_
#define RDB_PROTOCOL_CONFLICT_RESOLVER_HPP_

#include <functional>
#include <string>
#include <vector>

#include "containers/counted.hpp"
#include "rdb_protocol/cdc_types.hpp"

namespace ql {

class sym_t;
class func_term_t;
class raw_term_t;

enum class conflict_resolution_policy_t {
    LAST_WRITE_WINS,       // default
    SOURCE_WINS,
    TARGET_WINS,
    CUSTOM_HANDLER,        // ReQL function
    MANUAL                 // operator intervention
};

enum class handler_safety_level_t {
    RESTRICTED,   // No ReQL eval, only built-in resolvers
    PERMISSIVE    // Custom handler allowed (admin-only)
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
    // Throws if safety_level_ is RESTRICTED.
    void set_custom_handler(std::function<conflict_resolution_result_t(
        const change_record_t&, const change_record_t&)> handler);

    // Apply a resolved change to target datum, handling PK-merge semantics.
    // Called AFTER resolve() has picked the winning record.
    // Returns the merged datum_t, or null datum_t for DELETE.
    datum_t apply_merge(
        const change_record_t &resolved,
        const datum_t &target_current,
        bool target_exists) const;

    // Detect if source and target conflict on the same row.
    // Returns true if the changes touch the same primary key and the
    // operations could produce a different final state.
    bool detect_conflict(
        const change_record_t &source,
        const change_record_t &target) const;

    conflict_resolution_policy_t policy() const;

    // Safety level for custom handler execution.
    void set_safety_level(handler_safety_level_t level);
    handler_safety_level_t safety_level() const;

    // Validate that a custom conflict resolution handler is safe to execute.
    // Throws if the handler references disallowed ReQL terms (r.js, r.http,
    // r.db_create, r.db_drop, r.table_create, r.table_drop, r.do, r.branch)
    // or doesn't reference all of its arguments.
    static void validate_handler(
        const std::vector<sym_t> &used_syms,
        const counted_t<func_term_t> &handler_body);

    // Validate handler output structure and purity.
    // Calls validate_handler() then performs additional checks:
    // - Handler must be pure (no side-effects)
    // - Return must produce an object with recognized fields
    // - Must not access the global r namespace
    static void validate_handler_struct(
        const std::vector<sym_t> &used_syms,
        const counted_t<func_term_t> &handler_body);

private:
    conflict_resolution_policy_t policy_;
    handler_safety_level_t safety_level_ = handler_safety_level_t::PERMISSIVE;
    std::function<conflict_resolution_result_t(
        const change_record_t&, const change_record_t&)> custom_handler_;

    // LWW: compare (commit_timestamp, source_cluster_id, event_id.shard_id, event_id.lsn)
    static bool is_newer(const change_record_t &a, const change_record_t &b);

    // Walk a raw term tree recursively looking for forbidden ReQL terms.
    // Sets *error_out to a human-readable error message if a forbidden term is found.
    static void walk_for_forbidden_terms(
        const raw_term_t &term, std::string *error_out);
};

}  // namespace ql

#endif  // RDB_PROTOCOL_CONFLICT_RESOLVER_HPP_
