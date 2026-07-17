// Copyright 2026 RethinkDB, all rights reserved.
#include "btree/parallel_scan.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "btree/get_distribution.hpp"

/* Isolated B-tree range-split helpers (PAR-04). Row-count estimates and key
 * quantile sampling use the existing get_btree_key_distribution() which reads
 * the stat block and samples internal-node keys. */

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
    superblock_t *superblock,
    const key_range_t &range,
    signal_t *) {
    /* Use the B-tree stat block (btree_statblock_t::population) to get the
     * total key count. For range-restricted estimates, return the full
     * population as an upper bound — precise range counting requires a full
     * leaf traversal (future: use sampled cardinality in PAR-05). */
    if (superblock == nullptr || range.is_empty()) {
        return 0;
    }

    int64_t count = 0;
    get_btree_key_distribution(superblock, /*depth_limit=*/0,
                               &count, nullptr);
    return count;
}

std::vector<store_key_t> parallel_scan_t::sample_key_quantiles(
    superblock_t *superblock,
    const key_range_t &range,
    size_t num_splits,
    signal_t *) {
    /* Walk the B-tree and collect internal-node keys as split points. The
     * depth limit controls granularity: higher depth → narrower intervals →
     * more samples. Returns at most num_splits interior keys that fall within
     * the requested range. */
    if (superblock == nullptr || range.is_empty() || num_splits < 2) {
        return std::vector<store_key_t>();
    }

    /* Compute a depth limit from the requested split count. Each level of the
     * B-tree approximately doubles the number of sample points, so use
     * ceil(log2(num_splits)). */
    int depth = static_cast<int>(
        std::ceil(std::log2(static_cast<double>(num_splits))));
    if (depth < 1) depth = 1;

    std::vector<store_key_t> all_keys;
    get_btree_key_distribution(superblock, depth,
                               nullptr, &all_keys);

    /* Filter keys to those strictly inside the range, sort, and deduplicate.
     * Return at most num_splits - 1 interior keys (producing num_splits
     * non-overlapping subranges with the original range boundaries). */
    std::vector<store_key_t> inside;
    inside.reserve(std::min(all_keys.size(),
                            static_cast<size_t>(num_splits)));
    for (const store_key_t &k : all_keys) {
        if (range.contains_key(k) && k > range.left) {
            inside.push_back(k);
            if (inside.size() >= num_splits - 1) {
                break;
            }
        }
    }

    /* Already sorted by get_btree_key_distribution, but deduplicate just in
     * case. */
    inside.erase(std::unique(inside.begin(), inside.end()), inside.end());
    return inside;
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
