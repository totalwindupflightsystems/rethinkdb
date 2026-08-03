// Copyright 2026 RethinkDB, all rights reserved.
#include "btree/time_series_config.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "containers/archive/stl_types.hpp"
#include "rdb_protocol/time_series_errors.hpp"

namespace ql {

const downsample_step_t *time_series_config_t::select_downsample(
        uint64_t range_seconds) const {
    /* Order by target_interval descending without copying the steps (the
     * result must be a pointer into `downsample_steps`, not a temporary). */
    std::vector<size_t> order;
    order.reserve(downsample_steps.size());
    for (size_t i = 0; i < downsample_steps.size(); ++i) {
        order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(),
        [this](size_t a, size_t b) {
            return downsample_steps[a].target_interval_seconds
                > downsample_steps[b].target_interval_seconds;
        });
    for (size_t idx : order) {
        if (downsample_steps[idx].age_seconds < range_seconds) {
            return &downsample_steps[idx];
        }
    }
    return nullptr;
}

void time_series_config_t::validate_or_throw() const {
    if (enabled && time_field.empty()) {
        time_series_error::raise_logic(
            time_series_error::TIME_SERIES_FIELD_MISSING,
            "Time-series field must exist in every document.");
    }
    if (chunk_interval_seconds < 1) {
        time_series_error::raise_logic(
            time_series_error::TIME_SERIES_CONFIG_INVALID,
            "Time-series `chunk_interval` must be at least 1 second.");
    }
    if (retention_seconds > TIME_SERIES_MAX_RETENTION_SECONDS) {
        time_series_error::raise_logic(
            time_series_error::TIME_SERIES_RETENTION_EXCEEDED,
            "Retention period exceeds maximum allowed (365d).");
    }
    std::vector<uint64_t> ages;
    ages.reserve(downsample_steps.size());
    for (const downsample_step_t &step : downsample_steps) {
        if (step.age_seconds < 1) {
            time_series_error::raise_logic(
                time_series_error::TIME_SERIES_DOWNSAMPLE_CONFLICT,
                "Downsample `age` must be at least 1 second.");
        }
        if (step.target_interval_seconds < 1) {
            time_series_error::raise_logic(
                time_series_error::TIME_SERIES_DOWNSAMPLE_CONFLICT,
                "Downsample `to` must be at least 1 second.");
        }
        ages.push_back(step.age_seconds);
    }
    /* Two steps with the same `age` claim the same band of query ranges —
     * selection would be ambiguous, so the config is rejected. */
    std::sort(ages.begin(), ages.end());
    for (size_t i = 1; i < ages.size(); ++i) {
        if (ages[i] == ages[i - 1]) {
            time_series_error::raise_logic(
                time_series_error::TIME_SERIES_DOWNSAMPLE_CONFLICT,
                "Downsample age ranges must not overlap.");
        }
    }
    for (const downsample_step_t &step : downsample_steps) {
        if (step.aggregates.empty()) {
            time_series_error::raise_logic(
                time_series_error::TIME_SERIES_DOWNSAMPLE_CONFLICT,
                "Downsample `aggregate` must not be empty.");
        }
    }
}

RDB_IMPL_SERIALIZABLE_3_SINCE_v2_4(downsample_step_t,
    age_seconds, target_interval_seconds, aggregates);

RDB_IMPL_SERIALIZABLE_5_SINCE_v2_4(time_series_config_t,
    time_field, chunk_interval_seconds, retention_seconds,
    downsample_steps, enabled);

RDB_IMPL_EQUALITY_COMPARABLE_3(downsample_step_t,
    age_seconds, target_interval_seconds, aggregates);

RDB_IMPL_EQUALITY_COMPARABLE_5(time_series_config_t,
    time_field, chunk_interval_seconds, retention_seconds,
    downsample_steps, enabled);

}  // namespace ql
