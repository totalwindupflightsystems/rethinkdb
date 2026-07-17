// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/query_planner.hpp"

/* Stubs only (PAR-01). Eligibility, cost model, and decomposition land in
 * PAR-03. */

const char *parallel_serial_reason_string(parallel_serial_reason_t reason) {
    switch (reason) {
    case parallel_serial_reason_t::NOT_REQUESTED:
        return "not_requested";
    case parallel_serial_reason_t::BELOW_THRESHOLD:
        return "below_threshold";
    case parallel_serial_reason_t::INELIGIBLE_OPERATION:
        return "ineligible_operation";
    case parallel_serial_reason_t::INSUFFICIENT_RANGES:
        return "insufficient_ranges";
    case parallel_serial_reason_t::QUOTA_LIMITED:
        return "quota_limited";
    case parallel_serial_reason_t::CORO_POOL_EXHAUSTED:
        return "coro_pool_exhausted";
    case parallel_serial_reason_t::THREAD_AFFINITY:
        return "thread_affinity";
    case parallel_serial_reason_t::COST_NOT_BENEFICIAL:
        return "cost_not_beneficial";
    default:
        return "not_requested";
    }
}

parallel_planning_request_t::parallel_planning_request_t()
    : parallel_requested(false),
      max_workers(1),
      estimated_rows(0),
      estimated_bytes(0),
      estimated_serial_us(0),
      terminal(parallel_plan_t::terminal_t::STREAM),
      ordering(parallel_plan_t::ordering_t::UNORDERED),
      preferred_kind(query_fragment_t::kind_t::PRIMARY_KEY_RANGE) { }

parallel_cost_model_t::parallel_cost_model_t()
    : min_estimated_rows(10000),
      min_estimated_serial_us(100000),
      planning_us(0),
      startup_us_per_worker(0),
      merge_us_per_row(0),
      cross_thread_handoff_us(0) { }

query_planner_t::query_planner_t()
    : cost_model_() { }

query_planner_t::query_planner_t(parallel_cost_model_t cost_model)
    : cost_model_(cost_model) { }

query_planner_t::~query_planner_t() { }

parallel_planning_result_t query_planner_t::plan(
    const parallel_planning_request_t &) {
    /* Stub: always report serial until PAR-03. */
    parallel_planning_result_t result;
    result.serial_reason =
        parallel_serial_reason_string(parallel_serial_reason_t::NOT_REQUESTED);
    return result;
}

std::string query_planner_t::check_eligibility(
    const parallel_planning_request_t &) const {
    /* Stub: empty = eligible. */
    return std::string();
}

int64_t query_planner_t::estimate_parallel_us(
    const parallel_planning_request_t &request,
    size_t workers) const {
    /* Stub cost model. */
    if (workers == 0) {
        return request.estimated_serial_us;
    }
    return request.estimated_serial_us / static_cast<int64_t>(workers)
        + cost_model_.planning_us
        + cost_model_.startup_us_per_worker * static_cast<int64_t>(workers);
}

bool query_planner_t::is_beneficial(
    const parallel_planning_request_t &request,
    size_t workers) const {
    return estimate_parallel_us(request, workers) < request.estimated_serial_us;
}

std::vector<query_fragment_t> query_planner_t::make_range_fragments(
    const key_range_t &,
    const std::vector<store_key_t> &,
    query_fragment_t::kind_t,
    int64_t,
    int64_t) const {
    /* Stub — full half-open split logic in PAR-03. */
    return std::vector<query_fragment_t>();
}

const parallel_cost_model_t &query_planner_t::cost_model() const {
    return cost_model_;
}
