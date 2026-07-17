// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/query_planner.hpp"

#include <algorithm>
#include <limits>
#include <vector>

#include "containers/scoped.hpp"

/* Parallel query planner (PAR-03). Eligibility matrix, cost model, and
 * half-open range decomposition per
 * .coding-hermes/specs/phase3-parallel-query.md §4. */

namespace {

/* Benefit margin from spec §4.6: require ~15% predicted gain. */
constexpr int64_t benefit_margin_numer = 85;
constexpr int64_t benefit_margin_denom = 100;

/* Soft rows-per-worker target used when scaling worker count to estimated
 * cardinality (PAR-03; real quantiles arrive in PAR-04). */
constexpr int64_t rows_per_worker_divisor = 4;

/* Placeholder split-key alphabet (PAR-03): evenly-spaced single-byte
 * store_key_t values in [0x20, 0xE0] that fall inside the planned range.
 * Real B-tree quantiles replace this in PAR-04. */
constexpr int placeholder_split_lo = 0x20;
constexpr int placeholder_split_hi = 0xE0;

/* Serial-fallback / profile reason strings (spec §2.6 / §4.1). */
const char *const k_not_requested = "not_requested";
const char *const k_below_threshold = "below_threshold";
const char *const k_insufficient_workers = "insufficient_workers";
const char *const k_insufficient_ranges = "insufficient_ranges";
const char *const k_cost_not_beneficial = "cost_not_beneficial";

/* Eligibility matrix reasons (spec §4.2). */
const char *const k_reduce_not_parallelizable = "reduce_not_parallelizable";
const char *const k_operation_not_eligible = "operation_not_eligible";
const char *const k_fragment_overhead_dominates = "fragment_overhead_dominates";
const char *const k_external_merge_not_in_scope = "external_merge_not_in_scope";

/* Reserved for PAR-05 term-layer wiring:
 *   "writes_not_eligible", "changefeed_not_eligible",
 *   "nondeterministic_transform" */

/* Helper: serial-fallback result without a plan. */
parallel_planning_result_t serial_result(const char *reason) {
    return parallel_planning_result_t{
        scoped_ptr_t<parallel_plan_t>(),
        std::string(reason)};
}

parallel_planning_result_t serial_result(std::string reason) {
    return parallel_planning_result_t{
        scoped_ptr_t<parallel_plan_t>(),
        std::move(reason)};
}

int64_t proportional_share(int64_t total, size_t index, size_t count) {
    if (count == 0 || total <= 0) {
        return 0;
    }
    const int64_t n = static_cast<int64_t>(count);
    const int64_t i = static_cast<int64_t>(index);
    const int64_t start = (total * i) / n;
    const int64_t end = (total * (i + 1)) / n;
    return end - start;
}

/* Until PAR-04 provides B-tree quantiles, synthesize placeholder split points
 * by placing evenly-spaced single-byte keys in [0x20, 0xE0] that strictly
 * fall inside `range`. Returns at most (workers - 1) strictly increasing
 * interior keys. */
std::vector<store_key_t> synthesize_placeholder_splits(
        const key_range_t &range,
        size_t workers) {
    std::vector<store_key_t> splits;
    if (workers < 2 || range.is_empty()) {
        return splits;
    }

    const size_t want = workers - 1;
    splits.reserve(want);

    for (size_t i = 1; i <= want; ++i) {
        const int64_t span =
            static_cast<int64_t>(placeholder_split_hi - placeholder_split_lo);
        int byte_val = placeholder_split_lo
            + static_cast<int>((span * static_cast<int64_t>(i))
                / static_cast<int64_t>(want + 1));
        if (byte_val <= placeholder_split_lo) {
            byte_val = placeholder_split_lo + 1;
        }
        if (byte_val >= placeholder_split_hi) {
            byte_val = placeholder_split_hi - 1;
        }
        const uint8_t b = static_cast<uint8_t>(byte_val);
        const store_key_t mid(1, &b);

        if (!range.contains_key(mid) || !(mid > range.left)) {
            continue;
        }
        if (!splits.empty() && !(mid > splits.back())) {
            continue;
        }
        splits.push_back(mid);
        if (splits.size() >= want) {
            break;
        }
    }
    return splits;
}

}  // namespace

const char *parallel_serial_reason_string(parallel_serial_reason_t reason) {
    switch (reason) {
    case parallel_serial_reason_t::NOT_REQUESTED:
        return k_not_requested;
    case parallel_serial_reason_t::BELOW_THRESHOLD:
        return k_below_threshold;
    case parallel_serial_reason_t::INELIGIBLE_OPERATION:
        return "ineligible_operation";
    case parallel_serial_reason_t::INSUFFICIENT_RANGES:
        return k_insufficient_ranges;
    case parallel_serial_reason_t::QUOTA_LIMITED:
        return "quota_limited";
    case parallel_serial_reason_t::CORO_POOL_EXHAUSTED:
        return "coro_pool_exhausted";
    case parallel_serial_reason_t::THREAD_AFFINITY:
        return "thread_affinity";
    case parallel_serial_reason_t::COST_NOT_BENEFICIAL:
        return k_cost_not_beneficial;
    default:
        return k_not_requested;
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

/* ── Spec §4.2 eligibility matrix ─────────────────────────────────────────── */

std::string query_planner_t::check_eligibility(
        const parallel_planning_request_t &request) const {
    /* General reduce / fold / group — serial initially (spec §4.2 / §4.8).
     * COUNT/SUM/AVG/MIN/MAX use partial aggregation and remain eligible. */
    if (request.terminal == parallel_plan_t::terminal_t::REDUCE) {
        return k_reduce_not_parallelizable;
    }

    /* orderBy without primary-key fragment order needs external merge — serial
     * in Phase 3. PRIMARY_KEY_ASCENDING is compatible with range fragments. */
    if (request.ordering == parallel_plan_t::ordering_t::EXPLICIT_ORDER) {
        return k_external_merge_not_in_scope;
    }

    /* Writes/transactions, changefeeds, joins, distinct, union, zip, and point
     * lookups are not represented as eligible preferred_kind values. Term-layer
     * wiring (PAR-05) must not advertise those shapes as parallel requests;
     * unknown kinds fall through to the default below. */
    switch (request.preferred_kind) {
    case query_fragment_t::kind_t::PRIMARY_KEY_RANGE:
    case query_fragment_t::kind_t::FILTERED_PRIMARY_RANGE:
        break;
    case query_fragment_t::kind_t::PARTIAL_AGGREGATION:
        /* Partial aggregation requires a supported aggregate terminal. */
        if (request.terminal != parallel_plan_t::terminal_t::COUNT
                && request.terminal != parallel_plan_t::terminal_t::SUM
                && request.terminal != parallel_plan_t::terminal_t::AVG
                && request.terminal != parallel_plan_t::terminal_t::MIN
                && request.terminal != parallel_plan_t::terminal_t::MAX) {
            return k_operation_not_eligible;
        }
        break;
    case query_fragment_t::kind_t::SECONDARY_INDEX_RANGE:
        /* Eligible with restrictions: split only at complete scan keys
         * (secondary_key, primary_key) so equal-key runs are not divided
         * (spec §4.4). Placeholder splits used here are temporary until
         * storage-backed safe boundaries land in PAR-04. */
        break;
    default:
        /* distinct, union, zip, joins, and any unknown shape. */
        return k_operation_not_eligible;
    }

    /* Empty range / point-style empty work unit: fragment overhead dominates. */
    if (request.range.is_empty()) {
        return k_fragment_overhead_dominates;
    }

    return std::string();
}

/* ── Spec §4.1 planning flow ──────────────────────────────────────────────── */

parallel_planning_result_t query_planner_t::plan(
        const parallel_planning_request_t &request) {
    /* 1. Parallel opt-in (flowchart step B). */
    if (!request.parallel_requested) {
        return serial_result(k_not_requested);
    }

    /* 2. Eligibility matrix (step C). */
    {
        const std::string eligibility = check_eligibility(request);
        if (!eligibility.empty()) {
            return serial_result(eligibility);
        }
    }

    /* 3. Thresholds: need >= min rows OR >= min serial us (step E / §4.6). */
    if (request.estimated_rows < cost_model_.min_estimated_rows
            && request.estimated_serial_us < cost_model_.min_estimated_serial_us) {
        return serial_result(k_below_threshold);
    }

    if (request.range.is_empty()) {
        return serial_result(k_insufficient_ranges);
    }

    /* 4. Worker count: clamp to [2, hard_max], then soft-scale to rows. */
    size_t workers = request.max_workers;
    if (workers > server_parallel_workers_hard_max) {
        workers = server_parallel_workers_hard_max;
    }
    if (workers < 2) {
        return serial_result(k_insufficient_workers);
    }

    if (request.estimated_rows > 0 && cost_model_.min_estimated_rows > 0) {
        const int64_t target_per_worker = std::max<int64_t>(
            1, cost_model_.min_estimated_rows / rows_per_worker_divisor);
        int64_t by_rows = request.estimated_rows / target_per_worker;
        if (by_rows < 2) {
            by_rows = 2;
        }
        if (static_cast<size_t>(by_rows) < workers) {
            workers = static_cast<size_t>(by_rows);
        }
    }

    /* 5. Placeholder split points until PAR-04 quantiles (step F). */
    const std::vector<store_key_t> split_points =
        synthesize_placeholder_splits(request.range, workers);

    /* 6. Range decomposition into half-open fragments (step G). */
    std::vector<query_fragment_t> fragments = make_range_fragments(
        request.range,
        split_points,
        request.preferred_kind,
        request.estimated_rows,
        request.estimated_bytes);

    if (fragments.size() < 2) {
        return serial_result(k_insufficient_ranges);
    }

    workers = fragments.size();

    /* 7. Cost model must predict a beneficial plan (steps H/I). */
    if (!is_beneficial(request, workers)) {
        return serial_result(k_cost_not_beneficial);
    }

    /* 8. Complete parallel plan — construct result in the return so the
     * non-copyable scoped_ptr_t is moved, never copied. */
    const int64_t parallel_us = estimate_parallel_us(request, workers);
    return parallel_planning_result_t{
        make_scoped<parallel_plan_t>(
            std::move(fragments),
            request.ordering,
            request.terminal,
            request.estimated_serial_us,
            parallel_us),
        std::string()  /* empty serial_reason ⇒ plan is valid */
    };
}

/* ── Spec §4.6 cost model ─────────────────────────────────────────────────── */

int64_t query_planner_t::estimate_parallel_us(
        const parallel_planning_request_t &request,
        size_t workers) const {
    /* workers == 0: no parallel schedule; report serial cost. */
    if (workers == 0) {
        return request.estimated_serial_us;
    }

    const int64_t serial_us = std::max<int64_t>(0, request.estimated_serial_us);
    const int64_t rows = std::max<int64_t>(0, request.estimated_rows);
    const int64_t w = static_cast<int64_t>(workers);

    /* Spec §4.6 / task formula:
     *   parallel_us = planning_us
     *               + max(fragment_scan + fragment_filter)
     *               + merge_us_per_row * rows
     *               + startup_us * workers
     *               + cross_thread_cost
     *
     * serial_us / workers approximates max(fragment_scan_us + fragment_filter_us)
     * under even partition assumptions. The request already folds scan/filter/
     * encode costs into estimated_serial_us, so we do not re-derive them here. */
    int64_t parallel_us = cost_model_.planning_us;
    parallel_us += serial_us / w;
    parallel_us += cost_model_.startup_us_per_worker * w;
    parallel_us += cost_model_.merge_us_per_row * rows;
    parallel_us += cost_model_.cross_thread_handoff_us;

    if (parallel_us < 0) {
        return std::numeric_limits<int64_t>::max();
    }
    return parallel_us;
}

bool query_planner_t::is_beneficial(
        const parallel_planning_request_t &request,
        size_t workers) const {
    /* Spec §4.6:
     *   estimated_parallel_us < estimated_serial_us * 0.85 */
    if (workers < 2) {
        return false;
    }

    const int64_t serial_us = request.estimated_serial_us;
    if (serial_us <= 0) {
        return false;
    }

    const int64_t parallel_us = estimate_parallel_us(request, workers);
    return parallel_us
        < (serial_us * benefit_margin_numer) / benefit_margin_denom;
}

/* ── Spec §4.3 range decomposition ────────────────────────────────────────── */

std::vector<query_fragment_t> query_planner_t::make_range_fragments(
        const key_range_t &range,
        const std::vector<store_key_t> &split_points,
        query_fragment_t::kind_t kind,
        int64_t estimated_rows,
        int64_t estimated_bytes) const {
    std::vector<query_fragment_t> fragments;

    if (range.is_empty()) {
        return fragments;
    }

    /* Sort/dedup and keep only interior points of the half-open range. */
    std::vector<store_key_t> bounds = split_points;
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());

    std::vector<store_key_t> inside;
    inside.reserve(bounds.size());
    for (const store_key_t &b : bounds) {
        /* contains_key is [left, right): exclude left (would create an empty
         * leading fragment) and anything outside the original range. */
        if (range.contains_key(b) && b > range.left) {
            inside.push_back(b);
        }
    }

    /* Empty split list → single fragment covering the full original range
     * (preserves open/closed/unbounded semantics already normalized into
     * range.left / range.right). */
    if (inside.empty()) {
        fragments.push_back(query_fragment_t(
            0, kind, range, estimated_rows, estimated_bytes));
        return fragments;
    }

    /* Half-open subranges (spec §4.3):
     *   F0 = [range.left, split[0])
     *   Fi = [split[i-1], split[i])
     *   F(W-1) = [split[N-1], range.right)
     * First left and last right preserve the caller's bound semantics. */
    std::vector<key_range_t> subranges;
    subranges.reserve(inside.size() + 1);

    store_key_t left = range.left;
    for (size_t i = 0; i < inside.size(); ++i) {
        key_range_t frag(
            key_range_t::closed, left,
            key_range_t::open, inside[i]);
        if (!frag.is_empty()) {
            subranges.push_back(frag);
        }
        left = inside[i];
    }

    /* Last fragment: preserve original right bound (open / closed / none).
     * range.right is already the exclusive right bound (or unbounded). */
    if (range.right.unbounded) {
        key_range_t last(
            key_range_t::closed, left,
            key_range_t::none, store_key_t());
        if (!last.is_empty()) {
            subranges.push_back(last);
        }
    } else {
        key_range_t last(
            key_range_t::closed, left,
            key_range_t::open, range.right.key());
        if (!last.is_empty()) {
            subranges.push_back(last);
        }
    }

    const size_t n = subranges.size();
    fragments.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        fragments.push_back(query_fragment_t(
            i,
            kind,
            subranges[i],
            proportional_share(estimated_rows, i, n),
            proportional_share(estimated_bytes, i, n)));
    }
    return fragments;
}

const parallel_cost_model_t &query_planner_t::cost_model() const {
    return cost_model_;
}
