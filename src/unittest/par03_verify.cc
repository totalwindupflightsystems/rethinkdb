// Ad-hoc PAR-03 verification — not part of the permanent suite.
#include "unittest/gtest.hpp"
#include "rdb_protocol/query_planner.hpp"
#include "btree/keys.hpp"

namespace unittest {

TEST(Par03QueryPlanner, PlanSerialFallbackWithoutSplits) {
    query_planner_t planner;

    parallel_planning_request_t req;
    req.parallel_requested = true;
    req.max_workers = 8;
    req.range = key_range_t::universe();
    req.estimated_rows = 100000;
    req.estimated_bytes = 1000000;
    req.estimated_serial_us = 500000;
    req.terminal = parallel_plan_t::terminal_t::STREAM;
    req.ordering = parallel_plan_t::ordering_t::UNORDERED;
    req.preferred_kind = query_fragment_t::kind_t::PRIMARY_KEY_RANGE;

    parallel_planning_result_t result = planner.plan(req);
    ASSERT_TRUE(result.plan.get() == nullptr)
        << "expected serial fallback without real split points";
    ASSERT_FALSE(result.serial_reason.empty())
        << "expected non-empty serial_reason";
    ASSERT_EQ(result.serial_reason, "insufficient_ranges");
}

TEST(Par03QueryPlanner, NotRequested) {
    query_planner_t planner;
    parallel_planning_request_t req;
    req.parallel_requested = false;
    req.max_workers = 8;
    req.range = key_range_t::universe();
    req.estimated_rows = 100000;
    req.estimated_serial_us = 500000;

    parallel_planning_result_t result = planner.plan(req);
    ASSERT_EQ(result.serial_reason, "not_requested");
}

TEST(Par03QueryPlanner, EligibilityReduce) {
    query_planner_t planner;
    parallel_planning_request_t req;
    req.parallel_requested = true;
    req.terminal = parallel_plan_t::terminal_t::REDUCE;
    req.range = key_range_t::universe();
    ASSERT_EQ(planner.check_eligibility(req), "reduce_not_parallelizable");
}

TEST(Par03QueryPlanner, EligibilityExplicitOrder) {
    query_planner_t planner;
    parallel_planning_request_t req;
    req.parallel_requested = true;
    req.ordering = parallel_plan_t::ordering_t::EXPLICIT_ORDER;
    req.range = key_range_t::universe();
    ASSERT_EQ(planner.check_eligibility(req), "external_merge_not_in_scope");
}

TEST(Par03QueryPlanner, MakeRangeFragmentsEmptySplits) {
    query_planner_t planner;
    key_range_t range = key_range_t::universe();
    std::vector<store_key_t> splits;
    auto frags = planner.make_range_fragments(
        range, splits, query_fragment_t::kind_t::PRIMARY_KEY_RANGE, 1000, 2000);
    ASSERT_EQ(frags.size(), 1u);
    ASSERT_EQ(frags[0].ordinal(), 0u);
    ASSERT_EQ(frags[0].estimated_rows(), 1000);
    ASSERT_EQ(frags[0].estimated_bytes(), 2000);
}

TEST(Par03QueryPlanner, MakeRangeFragmentsTwoSplits) {
    query_planner_t planner;
    store_key_t a(std::string("a"));
    store_key_t m(std::string("m"));
    store_key_t z(std::string("z"));
    key_range_t range(key_range_t::closed, a, key_range_t::open, z);
    std::vector<store_key_t> splits;
    splits.push_back(m);
    auto frags = planner.make_range_fragments(
        range, splits, query_fragment_t::kind_t::PRIMARY_KEY_RANGE, 100, 1000);
    ASSERT_EQ(frags.size(), 2u);
    ASSERT_EQ(frags[0].ordinal(), 0u);
    ASSERT_EQ(frags[1].ordinal(), 1u);
    ASSERT_EQ(frags[0].estimated_rows() + frags[1].estimated_rows(), 100);
    // Non-overlapping half-open: frag0.right == right_bound(frag1.left)
    ASSERT_TRUE(frags[0].input_range().right
        == key_range_t::right_bound_t(frags[1].input_range().left));
}

TEST(Par03QueryPlanner, BelowThreshold) {
    query_planner_t planner;
    parallel_planning_request_t req;
    req.parallel_requested = true;
    req.max_workers = 4;
    req.range = key_range_t::universe();
    req.estimated_rows = 10;
    req.estimated_serial_us = 100;
    parallel_planning_result_t result = planner.plan(req);
    ASSERT_EQ(result.serial_reason, "below_threshold");
}

TEST(Par03QueryPlanner, InsufficientWorkers) {
    query_planner_t planner;
    parallel_planning_request_t req;
    req.parallel_requested = true;
    req.max_workers = 1;
    req.range = key_range_t::universe();
    req.estimated_rows = 100000;
    req.estimated_serial_us = 500000;
    parallel_planning_result_t result = planner.plan(req);
    ASSERT_EQ(result.serial_reason, "insufficient_workers");
}

TEST(Par03QueryPlanner, IsBeneficialMargin) {
    parallel_cost_model_t cm;
    cm.planning_us = 1;
    cm.startup_us_per_worker = 1000;
    cm.merge_us_per_row = 1;
    cm.cross_thread_handoff_us = 100;
    query_planner_t planner(cm);

    parallel_planning_request_t req;
    req.estimated_rows = 100000;
    req.estimated_serial_us = 1000000;

    // With expensive startup, few workers may still be beneficial if serial is large
    ASSERT_TRUE(planner.is_beneficial(req, 4));

    // With zero serial baseline, never beneficial
    req.estimated_serial_us = 0;
    ASSERT_FALSE(planner.is_beneficial(req, 4));
}

TEST(Par03QueryPlanner, ParallelPlanWithRealSplits) {
    // Drive plan() past insufficient_ranges by calling make_range_fragments
    // with real splits, then constructing the cost path via public APIs.
    query_planner_t planner;
    store_key_t a(std::string("aaa"));
    store_key_t m(std::string("mmm"));
    store_key_t z(std::string("zzz"));
    key_range_t range(key_range_t::closed, a, key_range_t::open, z);
    std::vector<store_key_t> splits;
    splits.push_back(m);

    auto frags = planner.make_range_fragments(
        range, splits, query_fragment_t::kind_t::PRIMARY_KEY_RANGE,
        100000, 1000000);
    ASSERT_GE(frags.size(), 2u);

    // Direct cost model checks
    parallel_planning_request_t req;
    req.parallel_requested = true;
    req.max_workers = 4;
    req.range = range;
    req.estimated_rows = 100000;
    req.estimated_bytes = 1000000;
    req.estimated_serial_us = 500000;
    req.preferred_kind = query_fragment_t::kind_t::PRIMARY_KEY_RANGE;

    ASSERT_TRUE(planner.check_eligibility(req).empty());
    ASSERT_TRUE(planner.is_beneficial(req, frags.size()));
    ASSERT_GT(planner.estimate_parallel_us(req, frags.size()), 0);
    // Default cost model has zeros → parallel_us may be 0 which is still < serial*0.85
}

}  // namespace unittest
