// Copyright 2026 RethinkDB, all rights reserved.
#include "btree/parallel_scan.hpp"

#include <algorithm>
#include <vector>

/* Isolated B-tree range-split helpers (PAR-03). Row-count / quantile sampling
 * remain stubs until PAR-04. */

parallel_scan_request_t::parallel_scan_request_t()
    : fragment_ordinal(0),
      estimated_rows(0),
      estimated_bytes(0) { }

parallel_scan_request_t::parallel_scan_request_t(
    size_t ordinal,
    key_range_t range,
    int64_t estimated_rows,
    int64_t estimated_bytes)
    : fragment_ordinal(ordinal),
      range(std::move(range)),
      estimated_rows(estimated_rows),
      estimated_bytes(estimated_bytes) { }

parallel_scan_state_t::parallel_scan_state_t()
    : fragment_ordinal(0),
      exhausted(true),
      rows_scanned(0),
      bytes_scanned(0) { }

parallel_scan_state_t::parallel_scan_state_t(
    const parallel_scan_request_t &request)
    : fragment_ordinal(request.fragment_ordinal),
      range(request.range),
      exhausted(false),
      rows_scanned(0),
      bytes_scanned(0) { }

int64_t parallel_scan_t::estimate_row_count(
    superblock_t *,
    const key_range_t &,
    signal_t *) {
    /* stub — PAR-04 wires B-tree estimates */
    return 0;
}

std::vector<store_key_t> parallel_scan_t::sample_key_quantiles(
    superblock_t *,
    const key_range_t &,
    size_t,
    signal_t *) {
    /* stub — PAR-04 wires B-tree quantile sampling */
    return std::vector<store_key_t>();
}

bool parallel_scan_t::validate_fragment_coverage(
    const key_range_t &original,
    const std::vector<key_range_t> &fragments) {
    /* Empty original: only valid with no non-empty fragments. */
    if (original.is_empty()) {
        for (const key_range_t &frag : fragments) {
            if (!frag.is_empty()) {
                return false;
            }
        }
        return true;
    }

    if (fragments.empty()) {
        return false;
    }

    /* Each fragment must be a non-empty subset of the original. */
    for (const key_range_t &frag : fragments) {
        if (frag.is_empty()) {
            return false;
        }
        if (!original.is_superset(frag)) {
            return false;
        }
    }

    /* Strict ordering + no gaps / no overlaps: half-open fragments must adjoin
     * exactly so fragment[i].right == right_bound_t(fragment[i+1].left). */
    for (size_t i = 0; i + 1 < fragments.size(); ++i) {
        key_range_t::right_bound_t next_left(fragments[i + 1].left);
        if (!(fragments[i].right == next_left)) {
            return false;
        }
        /* Defensive: also reject inverted order if equality check ever weakens. */
        if (fragments[i].right > next_left) {
            return false;
        }
    }

    /* Union equals original: first left and last right match. */
    if (fragments.front().left != original.left) {
        return false;
    }
    if (!(fragments.back().right == original.right)) {
        return false;
    }

    return true;
}

parallel_scan_split_t parallel_scan_t::split_range(
    const key_range_t &range,
    const std::vector<store_key_t> &boundaries) {
    parallel_scan_split_t result;

    if (range.is_empty()) {
        return result;
    }

    /* Sort and deduplicate boundaries. */
    std::vector<store_key_t> bounds = boundaries;
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());

    /* Keep only boundaries strictly inside the half-open range. A boundary at
     * range.left would produce an empty leading fragment; contains_key already
     * excludes the exclusive right bound. */
    std::vector<store_key_t> inside;
    inside.reserve(bounds.size());
    for (const store_key_t &b : bounds) {
        if (range.contains_key(b) && b > range.left) {
            inside.push_back(b);
        }
    }

    if (inside.empty()) {
        /* No interior splits: one fragment covering the whole range. */
        result.fragments.push_back(range);
        return result;
    }

    /* Build half-open subranges: [left, b0), [b0, b1), ..., [bN, right). */
    store_key_t left = range.left;
    for (size_t i = 0; i < inside.size(); ++i) {
        key_range_t frag(
            key_range_t::closed, left,
            key_range_t::open, inside[i]);
        if (!frag.is_empty()) {
            result.fragments.push_back(frag);
        }
        left = inside[i];
    }

    /* Last subrange preserves the original right bound semantics. */
    if (range.right.unbounded) {
        key_range_t last(
            key_range_t::closed, left,
            key_range_t::none, store_key_t());
        if (!last.is_empty()) {
            result.fragments.push_back(last);
        }
    } else {
        key_range_t last(
            key_range_t::closed, left,
            key_range_t::open, range.right.key());
        if (!last.is_empty()) {
            result.fragments.push_back(last);
        }
    }

    rassert(validate_fragment_coverage(range, result.fragments));
    return result;
}

parallel_scan_state_t parallel_scan_t::init_scan_state(
    const parallel_scan_request_t &request) {
    return parallel_scan_state_t(request);
}
