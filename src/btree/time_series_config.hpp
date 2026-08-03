// Copyright 2026 RethinkDB, all rights reserved.
#ifndef BTREE_TIME_SERIES_CONFIG_HPP_
#define BTREE_TIME_SERIES_CONFIG_HPP_

#include <cstdint>
#include <map>
#include <vector>

#include "containers/name_string.hpp"
#include "rdb_protocol/wire_func.hpp"
#include "rpc/semilattice/joins/macros.hpp"
#include "rpc/serialize_macros.hpp"

/* Time-series table configuration (Phase 3 spec §3.1).
 *
 * Stored as part of `table_config_t` (Raft metadata). The storage engine
 * consumes it in later phases (chunked B-trees, retention, downsampling);
 * this layer only defines the structure, its serialization, and validation.
 */

namespace ql {

/* Maximum allowed retention period: 365 days in seconds. Exactly-at-TTL is
 * accepted; strictly greater is rejected ("Retention period exceeds maximum
 * allowed (365d)."). */
static constexpr uint64_t TIME_SERIES_MAX_RETENTION_SECONDS = 31536000;

/* One downsampling rule: when a query range exceeds `age_seconds`, the
 * planner may substitute the precomputed aggregate at `target_interval_seconds`
 * resolution. `aggregates` maps output column name -> ReQL expression over
 * the row batch (e.g. {"avg_temp": r.avg("temperature")}). */
struct downsample_step_t {
    uint64_t age_seconds;             // e.g., 86400 for 24h
    uint64_t target_interval_seconds; // e.g., 60 for 1m
    std::map<name_string_t, wire_func_t> aggregates;

    RDB_DECLARE_ME_SERIALIZABLE(downsample_step_t);
};

class time_series_config_t {
public:
    name_string_t time_field;                 // e.g., "timestamp"
    uint64_t chunk_interval_seconds = 3600;   // default 1h
    uint64_t retention_seconds = 0;           // 0 = no retention
    std::vector<downsample_step_t> downsample_steps;
    bool enabled = false;

    RDB_DECLARE_ME_SERIALIZABLE(time_series_config_t);

    /* Returns the downsample step for a given query range, or nullptr.
     * Steps are considered ordered by target_interval descending; the first
     * step whose `age_seconds` is strictly less than `range_seconds` wins
     * (spec §4.3). Never returns a pointer into a temporary. */
    const downsample_step_t *select_downsample(uint64_t range_seconds) const;

    /* Semantic validation: retention bound, downsample step shape and
     * non-overlapping age ranges. Throws ql::exc_t (via the time-series
     * error catalog) on the first violation. */
    void validate_or_throw() const;
};

RDB_DECLARE_EQUALITY_COMPARABLE(downsample_step_t);
RDB_DECLARE_EQUALITY_COMPARABLE(time_series_config_t);

}  // namespace ql

#endif  // BTREE_TIME_SERIES_CONFIG_HPP_
