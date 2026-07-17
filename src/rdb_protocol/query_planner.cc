// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/query_planner.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "btree/parallel_scan.hpp"
#include "containers/scoped.hpp"

/* Parallel query planner (PAR-03). Eligibility matrix, range decomposition,
 * cost model, and serial fallback per
 * .coding-hermes/specs/phase3-parallel-query.md §4. */

namespace {

/* Spec §4.6 — require a predicted ~15% gain over serial to offset estimation
 * error. Integer form avoids floating point in the planner path. */
constexpr int64_t k_benefit_margin_numer = 85;
constexpr int64_t k_benefit_margin_denom = 100;

/* Placeholder split-key range (spec §4.3 / PAR-03): evenly-spaced
 * single-byte store_key_t values that fall inside the planned range.
 * Real B-tree quantiles arrive in PAR-04. */
constexpr int k_placeholder_split_lo = 0x20;
constexpr int k_placeholder_split_hi = 0xE0;

/* Soft rows-per-worker floor used when scaling worker count to estimated
 * cardinality. */
constexpr int64_t k_min_rows_per_worker = 1000;

/* Reserve a small planning-cost floor so the cost model always reflects
 * cross-fragment overhead. */
constexpr int64_t k_min_planning_us = 50;

/* ---------- helpers ------------------------------------------------ */

/* Distribute an estimate evenly across `n` fragments; the remainder
 * (from integer division) lands on the last fragment so the sum is
 * exact. */
void distribute_estimate(
    int64_t total, size_t n,
    std::vector<int64_t> *out) {
    if (n == 0 || total <= 0) {
        return;
    }
    int64_t each = total / static_cast<int64_t>(n);
    int64_t remainder = total % static_cast<int64_t>(n);
    for (size_t i = 0; i < n; ++i) {
        int64_t value = each + (i + 1 == n ? remainder : 0);
        out->push_back(value);
    }
}

/* Generate placeholder split points by emitting single-byte keys evenly
 * spaced across the printable range [0x20, 0xE0], filtering to those
 * that fall strictly inside `range`. Used until PAR-04 provides real
 * B-tree quantile estimates. */
std::vector<store_key_t> make_placeholder_splits(
    const key_range_t &range,
    size_t workers) {
    std::vector<store_key_t> splits;
    if (workers < 2 || range.is_empty()) {
        return splits;
    }

    const size_t num_candidates = std::min(workers, size_t{16});
    for (size_t i = 1; i < num_candidates; ++i) {
        int byte_val = k_placeholder_split_lo
            + static_cast<int>(
                (static_cast<double>(i) / static_cast<double>(num_candidates))
                * static_cast<double>(k_placeholder_split_hi
                                      - k_placeholder_split_lo));
        if (byte_val < k_placeholder_split_lo) {
            byte_val = k_placeholder_split_lo;
        }
        if (byte_val > k_placeholder_split_hi) {
            byte_val = k_placeholder_split_hi;
        }

        uint8_t buf[1] = { static_cast<uint8_t>(byte_val) };
        store_key_t candidate(1, buf);

        if (range.contains_key(candidate)
            && candidate > range.left
            && (splits.empty() || candidate > splits.back())) {
            splits.push_back(candidate);
        }
    }
    return splits;
}

/* Pick a concrete worker count from the request's max_workers, capped
 * by a soft cardinality-based rule. */
size_t choose_worker_count(
    const parallel_planning_request_t &request) {
    if (request.max_workers == 0) {
        return 0;
    }
    /* Never exceed requested max. */
    size_t w = request.max_workers;
    /* Soft cap: one worker per k_min_rows_per_worker estimated rows. */
    if (request.estimated_rows > 0) {
        size_t row_based = static_cast<size_t>(
            request.estimated_rows / k_min_rows_per_worker);
        if (row_based == 0) {
            row_based = 1;
        }
        w = std::min(w, row_based);
    }
    w = std::max(w, size_t{2});
    return w;
}

}  // anonymous namespace

/* ---------- parallel_serial_reason_string -------------------------- */

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

/* ---------- parallel_planning_request_t ----------------------------- */

parallel_planning_request_t::parallel_planning_request_t()
    : parallel_requested(false),
      max_workers(1),
      estimated_rows(0),
      estimated_bytes(0),
      estimated_serial_us(0),
      terminal(parallel_plan_t::terminal_t::STREAM),
      ordering(parallel_plan_t::ordering_t::UNORDERED),
      preferred_kind(query_fragment_t::kind_t::PRIMARY_KEY_RANGE) { }

/* ---------- parallel_cost_model_t ---------------------------------- */

parallel_cost_model_t::parallel_cost_model_t()
    : min_estimated_rows(10000),
      min_estimated_serial_us(100000),
      planning_us(0),
      startup_us_per_worker(0),
      merge_us_per_row(0),
      cross_thread_handoff_us(0) { }

/* ---------- query_planner_t ---------------------------------------- */

query_planner_t::query_planner_t()
    : cost_model_() { }

query_planner_t::query_planner_t(parallel_cost_model_t cost_model)
    : cost_model_(cost_model) { }

query_planner_t::~query_planner_t() { }

parallel_planning_result_t query_planner_t::plan(
    const parallel_planning_request_t &request) {
    /* 1. Not requested → serial. */
    if (!request.parallel_requested) {
        return parallel_planning_result_t{
            scoped_ptr_t<parallel_plan_t>(),
            parallel_serial_reason_string(
                parallel_serial_reason_t::NOT_REQUESTED)
        };
    }

    /* 2. Eligibility check. */
    std::string ineligible = check_eligibility(request);
    if (!ineligible.empty()) {
        return parallel_planning_result_t{
            scoped_ptr_t<parallel_plan_t>(),
            ineligible
        };
    }

    /* 3. Threshold check (§4.6). */
    if (request.estimated_rows < cost_model_.min_estimated_rows
        && request.estimated_serial_us < cost_model_.min_estimated_serial_us) {
        return parallel_planning_result_t{
            scoped_ptr_t<parallel_plan_t>(),
            parallel_serial_reason_string(
                parallel_serial_reason_t::BELOW_THRESHOLD)
        };
    }

    /* 4. Choose worker count. */
    size_t workers = choose_worker_count(request);
    if (workers < 2) {
        return parallel_planning_result_t{
            scoped_ptr_t<parallel_plan_t>(),
            parallel_serial_reason_string(
                parallel_serial_reason_t::INSUFFICIENT_RANGES)
        };
    }

    /* 5. Generate split points and fragments. */
    std::vector<store_key_t> splits = make_placeholder_splits(
        request.range, workers);
    if (splits.empty()) {
        return parallel_planning_result_t{
            scoped_ptr_t<parallel_plan_t>(),
            parallel_serial_reason_string(
                parallel_serial_reason_t::INSUFFICIENT_RANGES)
        };
    }

    std::vector<query_fragment_t> fragments =
        make_range_fragments(
            request.range, splits, request.preferred_kind,
            request.estimated_rows, request.estimated_bytes);

    if (fragments.empty()) {
        return parallel_planning_result_t{
            scoped_ptr_t<parallel_plan_t>(),
            parallel_serial_reason_string(
                parallel_serial_reason_t::INSUFFICIENT_RANGES)
        };
    }

    /* 6. Cost model check. */
    if (!is_beneficial(request, workers)) {
        return parallel_planning_result_t{
            scoped_ptr_t<parallel_plan_t>(),
            parallel_serial_reason_string(
                parallel_serial_reason_t::COST_NOT_BENEFICIAL)
        };
    }

    /* 7. Build the parallel plan. */
    int64_t est_parallel_us = estimate_parallel_us(request, workers);
    int64_t est_serial_us =
        request.estimated_serial_us > 0
        ? request.estimated_serial_us
        : estimate_parallel_us(request, 1);

    return parallel_planning_result_t{
        make_scoped<parallel_plan_t>(
            std::move(fragments),
            request.ordering,
            request.terminal,
            est_serial_us,
            est_parallel_us),
        std::string()
    };
}

std::string query_planner_t::check_eligibility(
    const parallel_planning_request_t &request) const {
    /* Primary-key range scans are always eligible. */
    /* For PAR-03 the eligibility table is intentionally narrow.
     * Writes, transactions, changefeeds, joins, and non-reduce
     * terminals are filtered out by the caller before building a
     * planning request, so we trust the preferred_kind + terminal
     * fields here. */

    /* PARTIAL_AGGREGATION via reduce/fold/group is serial in Phase 3. */
    if (request.terminal == parallel_plan_t::terminal_t::REDUCE) {
        return parallel_serial_reason_string(
            parallel_serial_reason_t::INELIGIBLE_OPERATION);
    }

    /* ORDERED plans requiring external merge are out of scope. */
    if (request.ordering == parallel_plan_t::ordering_t::EXPLICIT_ORDER) {
        return parallel_serial_reason_string(
            parallel_serial_reason_t::INELIGIBLE_OPERATION);
    }

    return std::string();
}

int64_t query_planner_t::estimate_parallel_us(
    const parallel_planning_request_t &request,
    size_t workers) const {
    if (workers == 0) {
        return request.estimated_serial_us > 0
            ? request.estimated_serial_us
            : (request.estimated_rows * 10);  /* fallback */
    }

    /* Spec §4.6 cost model:
     *   parallel_us = planning_us
     *               + max(fragment_scan_us + fragment_filter_us)
     *               + merge_us_per_row * estimated_rows
     *               + startup_us_per_worker * workers
     *               + cross_thread_handoff_us   */

    /* Per-worker scan + filter time (flat per-row cost / workers). */
    int64_t scan_us = request.estimated_rows;
    int64_t filter_us = request.estimated_rows;
    int64_t per_worker_us =
        (scan_us + filter_us) / static_cast<int64_t>(workers);

    /* Overhead terms. */
    int64_t overhead_us =
        std::max(cost_model_.planning_us, k_min_planning_us)
        + cost_model_.startup_us_per_worker * static_cast<int64_t>(workers)
        + cost_model_.merge_us_per_row * request.estimated_rows
        + cost_model_.cross_thread_handoff_us;

    return per_worker_us + overhead_us;
}

bool query_planner_t::is_beneficial(
    const parallel_planning_request_t &request,
    size_t workers) const {
    if (workers < 2) {
        return false;
    }

    int64_t serial_us = request.estimated_serial_us > 0
        ? request.estimated_serial_us
        : estimate_parallel_us(request, 1);

    int64_t parallel_us = estimate_parallel_us(request, workers);

    /* Spec §4.6 benefit margin: 15% gain required. */
    return parallel_us * k_benefit_margin_denom
        < serial_us * k_benefit_margin_numer;
}

std::vector<query_fragment_t> query_planner_t::make_range_fragments(
    const key_range_t &range,
    const std::vector<store_key_t> &split_points,
    query_fragment_t::kind_t kind,
    int64_t estimated_rows,
    int64_t estimated_bytes) const {
    std::vector<query_fragment_t> fragments;

    if (split_points.empty() || range.is_empty()) {
        return fragments;
    }

    /* Build the full boundary list: range.left, then each sorted split
     * point that falls strictly inside the range, then range.right. */
    std::vector<store_key_t> boundaries;
    boundaries.push_back(range.left);

    for (const store_key_t &sp : split_points) {
        if (sp > boundaries.back() && range.contains_key(sp)) {
            boundaries.push_back(sp);
        }
    }

    if (boundaries.size() < 2) {
        return fragments;
    }

    /* Each consecutive pair [b_i, b_{i+1}) forms one fragment. */
    const size_t num_fragments = boundaries.size() - 1;
    std::vector<int64_t> row_estimates;
    std::vector<int64_t> byte_estimates;
    distribute_estimate(estimated_rows, num_fragments, &row_estimates);
    distribute_estimate(estimated_bytes, num_fragments, &byte_estimates);

    for (size_t i = 0; i < num_fragments; ++i) {
        key_range_t fragment_range;
        if (i + 1 == num_fragments) {
            /* Preserve caller's right-bound semantics. */
            if (range.right.unbounded) {
                store_key_t dummy;
                fragment_range = key_range_t(
                    key_range_t::closed, boundaries[i],
                    key_range_t::none, dummy);
            } else {
                fragment_range = key_range_t(
                    key_range_t::closed, boundaries[i],
                    key_range_t::open, range.right.key());
            }
        } else {
            fragment_range = key_range_t(
                key_range_t::closed, boundaries[i],
                key_range_t::open, boundaries[i + 1]);
        }

        fragments.emplace_back(
            i, kind, fragment_range,
            row_estimates[i], byte_estimates[i]);
    }

    return fragments;
}

const parallel_cost_model_t &query_planner_t::cost_model() const {
    return cost_model_;
}
