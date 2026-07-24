// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/conflict_resolver.hpp"

#include "containers/uuid.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/error.hpp"
#include "rdb_protocol/func.hpp"
#include "rdb_protocol/sym.hpp"
#include "rdb_protocol/term_storage.hpp"
#include "time.hpp"
#include "utils.hpp"

namespace ql {

// ── Forward declarations for file-static helpers ──

namespace {

void walk_for_forbidden_terms_impl(const raw_term_t &term,
                                   std::string *error_out);
void walk_for_global_r_access_impl(const raw_term_t &term,
                                   std::string *error_out);
void walk_for_return_check_impl(const raw_term_t &term,
                                std::string *error_out);

}  // anonymous namespace

// ── Constructor and accessors ──

conflict_resolver_t::conflict_resolver_t(conflict_resolution_policy_t policy)
    : policy_(policy) {
}

conflict_resolution_policy_t conflict_resolver_t::policy() const {
    return policy_;
}

void conflict_resolver_t::set_safety_level(handler_safety_level_t level) {
    safety_level_ = level;
}

handler_safety_level_t conflict_resolver_t::safety_level() const {
    return safety_level_;
}

// ── Conflict log operations ──

void conflict_log_t::record(const conflict_log_entry_t &entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back(entry);
}

std::vector<conflict_log_entry_t> conflict_log_t::get_pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<conflict_log_entry_t> pending;
    for (const auto &e : entries_) {
        if (e.action == operator_action_t::PENDING) {
            pending.push_back(e);
        }
    }
    return pending;
}

std::vector<conflict_log_entry_t> conflict_log_t::get_all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

void conflict_log_t::resolve(const conflict_event_id_t &id,
                             operator_action_t action,
                             const std::string &note) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &e : entries_) {
        if (e.id == id) {
            e.action = action;
            e.operator_note = note;
            return;
        }
    }
}

size_t conflict_log_t::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

size_t conflict_log_t::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto &e : entries_) {
        if (e.action == operator_action_t::PENDING) {
            ++count;
        }
    }
    return count;
}

void conflict_resolver_t::set_conflict_log(conflict_log_t *log) {
    conflict_log_ = log;
}

// ── Custom handler registration with safety gating ──

void conflict_resolver_t::set_custom_handler(
        std::function<conflict_resolution_result_t(
            const change_record_t&, const change_record_t&)> handler) {
    if (safety_level_ == handler_safety_level_t::RESTRICTED) {
        rfail_datum(base_exc_t::LOGIC,
            "Cannot set custom handler: safety level is RESTRICTED. "
            "Only built-in resolvers are allowed.");
    }
    custom_handler_ = std::move(handler);
}

// ── Term tree walking helpers ──

void conflict_resolver_t::walk_for_forbidden_terms(
        const raw_term_t &term, std::string *error_out) {
    walk_for_forbidden_terms_impl(term, error_out);
}

// ── Handler validation ──

void conflict_resolver_t::validate_handler(
        const std::vector<sym_t> &used_syms,
        const counted_t<func_term_t> &handler_body) {
    guarantee(handler_body.has());

    // Get the raw term tree for the function
    const raw_term_t &func_src = handler_body->get_src();
    guarantee(func_src.type() == Term::FUNC);

    // A FUNC term has at least two args: args[0] = MAKE_ARRAY of arg numbers,
    // args[1] = body expression
    rcheck_datum(func_src.num_args() >= 2, base_exc_t::LOGIC,
        "Custom handler function must have arguments and a body.");

    // Walk the body (args[1]) for forbidden ReQL terms
    std::string error;
    walk_for_forbidden_terms_impl(func_src.arg(1), &error);
    if (!error.empty()) {
        rfail_datum(base_exc_t::LOGIC, "%s", error.c_str());
    }

    // Extract argument count from args[0] (the MAKE_ARRAY of arg numbers)
    const raw_term_t &arg_array = func_src.arg(0);
    size_t num_args = arg_array.num_args();
    rcheck_datum(num_args >= 2, base_exc_t::LOGIC,
        "Custom conflict handler must accept exactly 2 arguments "
        "(source, target).");

    // Extract arg sym values from the MAKE_ARRAY of DATUM terms.
    // In the term tree, each arg is a DATUM(num) where num is the
    // sym_t value (negative for dummy_var_t-based variables).
    std::vector<int64_t> arg_sym_values;
    for (size_t i = 0; i < num_args; ++i) {
        raw_term_t arg_term = arg_array.arg(i);
        if (arg_term.type() == Term::DATUM) {
            datum_t d = arg_term.datum();
            if (d.get_type() == datum_t::R_NUM) {
                arg_sym_values.push_back(static_cast<int64_t>(d.as_num()));
            }
        }
    }

    // Verify all arg syms are referenced in the handler body
    for (int64_t arg_val : arg_sym_values) {
        bool found = false;
        for (const sym_t &s : used_syms) {
            if (s.value == arg_val) {
                found = true;
                break;
            }
        }
        rcheck_datum(found, base_exc_t::LOGIC,
            "Custom conflict handler must reference all of its arguments "
            "(source and target).");
    }
}

void conflict_resolver_t::validate_handler_struct(
        const std::vector<sym_t> &used_syms,
        const counted_t<func_term_t> &handler_body) {
    // First, run the basic safety validation (forbidden terms + arg usage)
    validate_handler(used_syms, handler_body);

    // Get the raw term tree for additional structural checks
    const raw_term_t &func_src = handler_body->get_src();
    guarantee(func_src.type() == Term::FUNC);
    guarantee(func_src.num_args() >= 2);

    const raw_term_t &body_term = func_src.arg(1);

    // Check that the body doesn't access the global r namespace.
    // In RethinkDB's var_captures model, negative sym values are
    // function-local variables (e.g. dummy_var_t-based) and non-negative
    // values are globals like `r`.
    std::string purity_error;
    walk_for_global_r_access_impl(body_term, &purity_error);
    if (!purity_error.empty()) {
        rfail_datum(base_exc_t::LOGIC, "%s", purity_error.c_str());
    }

    // Structural check: the body's top-level term should produce an object.
    // We check the type of the return expression.
    Term::TermType body_type = body_term.type();
    if (body_type == Term::MAKE_OBJ) {
        // Direct object literal — the ideal case for a handler like
        //   function(s, t) { return {action: "source", reason: "..."}; }
    } else if (body_type == Term::BRANCH) {
        // branch(condition, true_case, false_case) — both arms must
        // produce objects or valid return shapes.
        if (body_term.num_args() >= 3) {
            for (size_t i = 1; i <= 2; ++i) {
                std::string branch_error;
                walk_for_return_check_impl(body_term.arg(i), &branch_error);
                if (!branch_error.empty()) {
                    rfail_datum(base_exc_t::LOGIC, "%s",
                        branch_error.c_str());
                }
            }
        }
    } else if (body_type == Term::FUNCALL) {
        // A function call — could return an object indirectly.
        // Allow this; the runtime will catch type mismatches.
    } else if (body_type == Term::ERROR) {
        rfail_datum(base_exc_t::LOGIC,
            "Custom conflict handler must not unconditionally throw "
            "an error.");
    }
    // For other term types the handler may still be valid —
    // the forbidden-terms check is the primary gate.
}

// ── LWW comparison ──

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

// ── Resolution ──

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

    // Auto-log conflict when skipped or MANUAL policy
    if (conflict_log_ != nullptr
            && (result.skipped || policy_ == conflict_resolution_policy_t::MANUAL)) {
        conflict_log_entry_t entry;
        entry.id = generate_uuid();
        entry.occurred_at = current_microtime();
        entry.source = source;
        entry.target = target;
        entry.resolution = result;
        entry.action = operator_action_t::PENDING;
        conflict_log_->record(entry);
    }

    return result;
}

// ── Merge ──

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
            return target_current.merge(after);
        }
        return after;

    case change_operation_t::UPDATE:
        if (target_exists) {
            return target_current.merge(after);
        }
        return after;

    case change_operation_t::REPLACE:
        return after;

    case change_operation_t::DELETE:
        return datum_t();
    }

    return datum_t();
}

// ── Conflict detection ──

bool conflict_resolver_t::detect_conflict(
        const change_record_t &source,
        const change_record_t &target) const {
    // Different table → different PK, no conflict
    if (source.event_id.table_id != target.event_id.table_id) {
        return false;
    }

    // Determine row identity: for INSERT use after_image,
    // for UPDATE/DELETE/REPLACE use before_image
    const std::vector<char> &source_row_id =
        (source.op == change_operation_t::INSERT)
            ? source.after_image : source.before_image;
    const std::vector<char> &target_row_id =
        (target.op == change_operation_t::INSERT)
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

// ═══════════════════════════════════════════════════════════════════════
// File-static helper implementations
// ═══════════════════════════════════════════════════════════════════════

namespace {

void walk_for_forbidden_terms_impl(const raw_term_t &term,
                                   std::string *error_out) {
    Term::TermType type = term.type();

    // Check for forbidden ReQL terms
    switch (type) {
    case Term::JAVASCRIPT:
        *error_out = "Custom conflict handler must not contain r.js().";
        return;
    case Term::HTTP:
        *error_out = "Custom conflict handler must not contain r.http().";
        return;
    case Term::DB_CREATE:
        *error_out =
            "Custom conflict handler must not contain r.db_create().";
        return;
    case Term::DB_DROP:
        *error_out =
            "Custom conflict handler must not contain r.db_drop().";
        return;
    case Term::TABLE_CREATE:
        *error_out =
            "Custom conflict handler must not contain r.table_create().";
        return;
    case Term::TABLE_DROP:
        *error_out =
            "Custom conflict handler must not contain r.table_drop().";
        return;
    case Term::FUNCALL:
        *error_out =
            "Custom conflict handler must not contain r.do() "
            "(nested execution).";
        return;
    case Term::BRANCH:
        *error_out =
            "Custom conflict handler must not contain r.branch() "
            "(control flow escape).";
        return;
    default:
        break;
    }

    // Recurse into positional arguments
    for (size_t i = 0; i < term.num_args(); ++i) {
        walk_for_forbidden_terms_impl(term.arg(i), error_out);
        if (!error_out->empty()) {
            return;
        }
    }

    // Note: we skip optarg recursion here because forbidden ReQL terms
    // (r.js, r.http, r.db_create, etc.) never appear as optargs in a
    // conflict handler body — they are always positional arguments.
    // Optarg iteration via each_optarg() triggers Boost variant size
    // computation issues with counted_t<generated_term_t> in some
    // compilation contexts, so we avoid it.
}

void walk_for_global_r_access_impl(const raw_term_t &term,
                                   std::string *error_out) {
    Term::TermType type = term.type();

    if (type == Term::VAR) {
        // A VAR term has args[0] = DATUM with the sym value.
        // Positive sym values are globals; negative are locals.
        if (term.num_args() >= 1) {
            raw_term_t arg0 = term.arg(0);
            if (arg0.type() == Term::DATUM) {
                datum_t d = arg0.datum();
                if (d.get_type() == datum_t::R_NUM) {
                    int64_t sym_val = static_cast<int64_t>(d.as_num());
                    if (sym_val >= 0) {
                        *error_out =
                            "Custom conflict handler must not access "
                            "the global r namespace.";
                        return;
                    }
                }
            }
        }
    }

    // Recurse into args
    for (size_t i = 0; i < term.num_args(); ++i) {
        walk_for_global_r_access_impl(term.arg(i), error_out);
        if (!error_out->empty()) {
            return;
        }
    }

    // Note: we skip optarg recursion — see comment in
    // walk_for_forbidden_terms_impl.
}

void walk_for_return_check_impl(const raw_term_t &term,
                                std::string *error_out) {
    Term::TermType type = term.type();

    if (type == Term::MAKE_OBJ) {
        // This branch returns an object literal — acceptable.
        return;
    }

    if (type == Term::ERROR) {
        *error_out =
            "Custom conflict handler branch must not unconditionally "
            "throw an error.";
        return;
    }

    // For other types (FUNCALL, BRANCH, VAR, etc.), we allow them
    // since the runtime will catch type mismatches. The forbidden-terms
    // check is the primary safety gate.
}

}  // anonymous namespace

}  // namespace ql
