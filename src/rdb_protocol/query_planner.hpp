// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_QUERY_PLANNER_HPP_
#define RDB_PROTOCOL_QUERY_PLANNER_HPP_

/* Parallel query planner types (Phase 3 / PAR-01).

Planning input/output types, eligibility, and cost-model interfaces. Logic
lands in later phases (PAR-03). Fragment/plan types live in
parallel_executor.hpp (§3.2–§3.3).

See .coding-hermes/specs/phase3-parallel-query.md §3.3, §4. */

#include <cstdint>
#include <string>
#include <vector>

#include "btree/keys.hpp"
#include "errors.hpp"
#include "rdb_protocol/parallel_executor.hpp"

/* Why the planner selected serial execution (profile fallback_reason). */
enum class parallel_serial_reason_t {
    NOT_REQUESTED,
    BELOW_THRESHOLD,
    INELIGIBLE_OPERATION,
    INSUFFICIENT_RANGES,
    QUOTA_LIMITED,
    CORO_POOL_EXHAUSTED,
    THREAD_AFFINITY,
    COST_NOT_BENEFICIAL
};

/* Converts a serial reason to the profile string from spec §2.6. */
const char *parallel_serial_reason_string(parallel_serial_reason_t reason);

/* Immutable input to the parallel planner (§4). */
struct parallel_planning_request_t {
    bool parallel_requested;
    size_t max_workers;
    key_range_t range;
    int64_t estimated_rows;
    int64_t estimated_bytes;
    int64_t estimated_serial_us;
    parallel_plan_t::terminal_t terminal;
    parallel_plan_t::ordering_t ordering;
    query_fragment_t::kind_t preferred_kind;

    parallel_planning_request_t();
};

/* Cost-model parameters (configurable guardrails from §2.4 / §4.6). */
struct parallel_cost_model_t {
    int64_t min_estimated_rows;
    int64_t min_estimated_serial_us;
    int64_t planning_us;
    int64_t startup_us_per_worker;
    int64_t merge_us_per_row;
    int64_t cross_thread_handoff_us;

    parallel_cost_model_t();
};

/* query_planner_t (§4.1) — decides serial vs parallel and emits a complete
 * plan or a serial_reason. Data-structures / interface stubs only. */
class query_planner_t {
public:
    query_planner_t();
    explicit query_planner_t(parallel_cost_model_t cost_model);
    ~query_planner_t();

    DISABLE_COPYING(query_planner_t);

    /* Produce either a complete parallel_plan_t or a serial_reason. */
    parallel_planning_result_t plan(const parallel_planning_request_t &request);

    /* Eligibility matrix check (§4.2). Empty string means eligible. */
    std::string check_eligibility(
        const parallel_planning_request_t &request) const;

    /* Cost model (§4.6). */
    int64_t estimate_parallel_us(
        const parallel_planning_request_t &request,
        size_t workers) const;

    bool is_beneficial(
        const parallel_planning_request_t &request,
        size_t workers) const;

    /* Range decomposition helpers (boundaries only — no storage I/O). */
    std::vector<query_fragment_t> make_range_fragments(
        const key_range_t &range,
        const std::vector<store_key_t> &split_points,
        query_fragment_t::kind_t kind,
        int64_t estimated_rows,
        int64_t estimated_bytes) const;

    const parallel_cost_model_t &cost_model() const;

private:
    parallel_cost_model_t cost_model_;
};

#endif  // RDB_PROTOCOL_QUERY_PLANNER_HPP_
