// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/brin.hpp"

#include <string>
#include <vector>

#include "containers/archive/stl_types.hpp"
#include "containers/archive/versioned.hpp"

bool validate_brin_summary(const brin_summary_t &summary,
                           const std::vector<std::string> &columns,
                           std::string *error_out) {
    const size_t n = columns.size();
    if (summary.minimum.size() != n || summary.maximum.size() != n) {
        if (error_out != nullptr) {
            *error_out = "BRIN summary min/max size does not match column count";
        }
        return false;
    }

    /* When the right bound is finite, left must be strictly less than it so the
     * half-open key range [left, right) is non-empty. */
    if (!summary.primary_key_right.unbounded
        && !(summary.primary_key_left < summary.primary_key_right.key())) {
        if (error_out != nullptr) {
            *error_out = "BRIN summary primary_key_left is not less than "
                         "primary_key_right";
        }
        return false;
    }

    for (size_t i = 0; i < n; ++i) {
        if (!summary.minimum[i].has() || !summary.maximum[i].has()) {
            if (error_out != nullptr) {
                *error_out = "BRIN summary min/max contains an uninitialized datum";
            }
            return false;
        }
        if (summary.minimum[i].cmp(summary.maximum[i]) > 0) {
            if (error_out != nullptr) {
                *error_out = "BRIN summary minimum is greater than maximum";
            }
            return false;
        }
    }

    /* live_row_count is uint64_t; always >= 0. */
    return true;
}

bool validate_brin_index(const brin_index_t &index, std::string *error_out) {
    if (index.format_version != BRIN_FORMAT_VERSION) {
        if (error_out != nullptr) {
            *error_out = "BRIN format_version is not 1";
        }
        return false;
    }

    if (index.range_size == 0
        || index.range_size < BRIN_RANGE_SIZE_MIN
        || index.range_size > BRIN_RANGE_SIZE_MAX) {
        if (error_out != nullptr) {
            *error_out = "BRIN range_size must be in [16, 65536]";
        }
        return false;
    }

    if (index.columns.empty()) {
        if (error_out != nullptr) {
            *error_out = "BRIN columns must be non-empty";
        }
        return false;
    }

    for (size_t i = 0; i < index.columns.size(); ++i) {
        if (index.columns[i].empty()) {
            if (error_out != nullptr) {
                *error_out = "BRIN column names must be non-empty strings";
            }
            return false;
        }
    }

    for (size_t i = 0; i < index.summaries.size(); ++i) {
        std::string summary_err;
        if (!validate_brin_summary(index.summaries[i], index.columns,
                                   &summary_err)) {
            if (error_out != nullptr) {
                *error_out = summary_err;
            }
            return false;
        }
    }

    /* Summaries must be sorted and non-overlapping: the exclusive right of
     * summary i must be <= the left of summary i+1 (as a right_bound_t).
     * Adjacent ranges that meet at a key are allowed; overlapping or out of
     * order ranges are rejected. */
    for (size_t i = 0; i + 1 < index.summaries.size(); ++i) {
        const brin_summary_t &cur = index.summaries[i];
        const brin_summary_t &next = index.summaries[i + 1];
        if (cur.primary_key_right.unbounded) {
            if (error_out != nullptr) {
                *error_out = "BRIN summaries after an unbounded right bound";
            }
            return false;
        }
        /* next.left must be >= cur.right (exclusive right means next may start
         * exactly at cur.right.key()). Spec phrasing "left of i+1 > right of i"
         * is satisfied for exclusive right bounds by right_bound comparison. */
        key_range_t::right_bound_t next_left(next.primary_key_left);
        if (!(cur.primary_key_right <= next_left)) {
            if (error_out != nullptr) {
                *error_out = "BRIN summaries are unsorted or overlapping";
            }
            return false;
        }
    }

    return true;
}
