// Copyright 2026 RethinkDB, all rights reserved.
#ifndef BTREE_PARALLEL_SCAN_HPP_
#define BTREE_PARALLEL_SCAN_HPP_

/* Storage-facing parallel fragment scan types (Phase 3 / PAR-01).

Fragment-local B-tree scan request, key-range invariants, and
sampling/estimate interfaces used by the query planner. Scan logic lands in
later phases (PAR-04).

Fragment boundaries are half-open [start, end). Adjacent fragments meet at a
boundary without omission or duplication.

See .coding-hermes/specs/phase3-parallel-query.md §5.1. */

#include <cstdint>
#include <vector>

#include "btree/keys.hpp"
#include "errors.hpp"

class superblock_t;
class signal_t;

/* One fragment-local scan request handed to the storage layer. */
struct parallel_scan_request_t {
    size_t fragment_ordinal;
    key_range_t range;
    int64_t estimated_rows;
    int64_t estimated_bytes;

    parallel_scan_request_t();
    parallel_scan_request_t(
        size_t ordinal,
        key_range_t range,
        int64_t estimated_rows,
        int64_t estimated_bytes);
};

/* Result of splitting a key range into non-overlapping half-open fragments. */
struct parallel_scan_split_t {
    std::vector<key_range_t> fragments;
};

/* Per-fragment cursor setup state owned by one worker. Never shared across
 * workers (no shared B-tree cursor / page lock). */
struct parallel_scan_state_t {
    size_t fragment_ordinal;
    key_range_t range;
    store_key_t cursor_position;
    bool exhausted;
    uint64_t rows_scanned;
    int64_t bytes_scanned;

    parallel_scan_state_t();
    explicit parallel_scan_state_t(const parallel_scan_request_t &request);
};

/* Storage-layer helpers for fragment scans and planner estimates (§5.1). */
class parallel_scan_t {
public:
    /* Estimate row count in `range` for cost modeling. Stub returns 0. */
    static int64_t estimate_row_count(
        superblock_t *superblock,
        const key_range_t &range,
        signal_t *interruptor);

    /* Sample approximate key quantiles for range decomposition. Stub returns
     * an empty vector. */
    static std::vector<store_key_t> sample_key_quantiles(
        superblock_t *superblock,
        const key_range_t &range,
        size_t num_splits,
        signal_t *interruptor);

    /* Validate half-open non-overlap and coverage invariants:
     *  - fragments strictly ordered
     *  - no fragment intersects another
     *  - union equals original range
     * Stub returns true. */
    static bool validate_fragment_coverage(
        const key_range_t &original,
        const std::vector<key_range_t> &fragments);

    /* Split `range` at `boundaries` into half-open subranges [b_i, b_{i+1}).
     * Stub returns empty split. */
    static parallel_scan_split_t split_range(
        const key_range_t &range,
        const std::vector<store_key_t> &boundaries);

    /* Build a fragment-local scan state from a request. */
    static parallel_scan_state_t init_scan_state(
        const parallel_scan_request_t &request);

private:
    /* Static helpers only — not instantiable. */
    parallel_scan_t();
    DISABLE_COPYING(parallel_scan_t);
};

#endif  // BTREE_PARALLEL_SCAN_HPP_
