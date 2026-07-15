// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_BRIN_HPP_
#define RDB_PROTOCOL_BRIN_HPP_

#include <cstdint>
#include <string>
#include <vector>

#include "btree/keys.hpp"
#include "containers/archive/archive.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/serialize_datum.hpp"
#include "rpc/serialize_macros.hpp"

/* BRIN (Block Range Index) summary types.
 *
 * A BRIN-like secondary index stores min/max extrema over contiguous ranges of
 * the primary-key B-tree rather than one sindex entry per row. See
 * .coding-hermes/research/brin-index-design.md for the full design.
 *
 * Persistence: `brin_index_t` is the sidecar blob written with
 * serialize_onto_blob / serialize_for_metainfo style versioned serialization
 * (same mechanism as the HNSW graph sidecar).
 */

/* Only format_version 1 is supported in Phase 1. */
static constexpr uint32_t BRIN_FORMAT_VERSION = 1;

/* Valid range_size bounds for BRIN construction (rows per logical range). */
static constexpr uint64_t BRIN_RANGE_SIZE_MIN = 16;
static constexpr uint64_t BRIN_RANGE_SIZE_MAX = 65536;

/* Default range size when the client omits range_size. */
static constexpr uint64_t BRIN_RANGE_SIZE_DEFAULT = 128;

/* One range summary: min/max over a primary-key interval. */
struct brin_summary_t {
    /* Inclusive left bound of the primary-key range to scan. */
    store_key_t primary_key_left;
    /* Exclusive (or unbounded) right bound of the primary-key range. */
    key_range_t::right_bound_t primary_key_right;
    /* Per-column minimums; size must equal columns.size(). */
    std::vector<ql::datum_t> minimum;
    /* Per-column maximums; size must equal columns.size(). */
    std::vector<ql::datum_t> maximum;
    /* Approximate live (non-null-mapping) row count in this range. */
    uint64_t live_row_count;
    /* Rows whose mapping returned null / no indexable value. */
    uint64_t null_row_count;
    /* True when a delete/update expanded extrema beyond a fresh rescan.
     * Never means the summary is unsafe for pruning. */
    bool dirty;

    brin_summary_t()
        : live_row_count(0),
          null_row_count(0),
          dirty(false) { }

    RDB_MAKE_ME_SERIALIZABLE_7(brin_summary_t,
        primary_key_left, primary_key_right, minimum, maximum,
        live_row_count, null_row_count, dirty);
};

/* Full BRIN index sidecar blob stored on the sindex superblock. */
struct brin_index_t {
    uint32_t format_version;
    /* Construction/maintenance target: live rows per logical range. */
    uint64_t range_size;
    /* Column names (Phase 1: exactly one); establishes summary component count. */
    std::vector<std::string> columns;
    /* Sorted, non-overlapping primary-key range summaries. */
    std::vector<brin_summary_t> summaries;

    brin_index_t()
        : format_version(BRIN_FORMAT_VERSION),
          range_size(0) { }

    RDB_MAKE_ME_SERIALIZABLE_4(brin_index_t,
        format_version, range_size, columns, summaries);
};

/* Validate a single summary against the index column schema.
 * On failure writes a human-readable message to *error_out (if non-null). */
bool validate_brin_summary(const brin_summary_t &summary,
                           const std::vector<std::string> &columns,
                           std::string *error_out);

/* Validate an entire brin_index_t (format, range_size, columns, summaries).
 * On failure writes a human-readable message to *error_out (if non-null). */
bool validate_brin_index(const brin_index_t &index, std::string *error_out);

#endif  // RDB_PROTOCOL_BRIN_HPP_
