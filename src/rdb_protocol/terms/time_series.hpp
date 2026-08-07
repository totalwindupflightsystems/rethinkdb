// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_TERMS_TIME_SERIES_HPP_
#define RDB_PROTOCOL_TERMS_TIME_SERIES_HPP_

#include "btree/time_chunk.hpp"
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

/* Format the durable chunk index into its config() datum shape
 * (PHASE3-TS-2):
 *   {chunk_count, total_rows, newest: {min_time_us, max_time_us, row_count},
 *    chunks: [{min_time_us, max_time_us, row_count}, ...]}
 * `newest` is null when there are no chunks yet; `chunks` is empty then.
 * Timestamps are epoch microseconds as numbers (double-precision integers
 * up to 2^53 — safe for micros since 1970). */
datum_t format_time_series_chunk_info_datum(
        const time_chunk_index_t &chunk_index, bool has_catalog);

/* PHASE3-TS-4: tableReconfigure support (spec §6.1). Returns true when
 * `new_datum` is a formatted time-series config datum that differs from
 * `old_datum` at most in `retention` — the one mutable time-series
 * option. `field`, `chunk_interval` and `downsample` stay immutable;
 * identical datums are trivially allowed, and anything that is not an
 * object with the same key set (including null / missing configs) is
 * rejected. */
bool time_series_reconfigure_allows_retention_change(
        const datum_t &old_datum, const datum_t &new_datum);

}  // namespace ql

#endif  // RDB_PROTOCOL_TERMS_TIME_SERIES_HPP_
