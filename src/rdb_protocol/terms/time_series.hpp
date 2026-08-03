// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_TERMS_TIME_SERIES_HPP_
#define RDB_PROTOCOL_TERMS_TIME_SERIES_HPP_

#include "btree/time_series_config.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/term_storage.hpp"

namespace ql {

/* Parse the raw `timeSeries` tableCreate optarg term into a config.
 *
 * The optarg is parsed from its RAW term (not evaluated) because the
 * downsample `aggregate` values are ReQL expressions (e.g.
 * r.avg("temperature")) which cannot be evaluated to datums — the same
 * reason generated columns parse their object raw. Scalar keys (`field`,
 * `chunk_interval`, `retention`, `age`, `to`) must be literal datums.
 *
 * Structural errors (wrong types, missing keys) raise via `target`;
 * semantic validation (retention bound, overlapping downsample ages) is
 * delegated to time_series_config_t::validate_or_throw().
 */
time_series_config_t parse_time_series_config_from_raw_term(
        const raw_term_t &ts_term, const rcheckable_t *target);

/* Format a time-series config into its config() datum shape:
 *   {field, chunk_interval, retention, downsample: [{age, to, aggregate}]}
 * where aggregate expressions are rendered as JS source strings (same
 * representation as generated columns). */
datum_t format_time_series_config_datum(const time_series_config_t &config);

}  // namespace ql

#endif  // RDB_PROTOCOL_TERMS_TIME_SERIES_HPP_
