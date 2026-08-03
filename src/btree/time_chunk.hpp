// Copyright 2026 RethinkDB, all rights reserved.
#ifndef BTREE_TIME_CHUNK_HPP_
#define BTREE_TIME_CHUNK_HPP_

#include <cstdint>
#include <vector>

#include "rpc/semilattice/joins/macros.hpp"
#include "rpc/serialize_macros.hpp"

/* Time-chunk metadata (Phase 3 spec §3.2).
 *
 * Each time-series chunk is a self-contained sub-B-tree; the chunk index is
 * the BRIN-like sparse index used for read pruning and retention. Bounds are
 * half-open: [min_time_us, max_time_us). */

namespace ql {

struct time_chunk_bounds_t {
    uint64_t min_time_us;  // micros since epoch
    uint64_t max_time_us;  // exclusive upper bound
    uint64_t row_count;

    RDB_DECLARE_ME_SERIALIZABLE(time_chunk_bounds_t);
};

class time_chunk_index_t {
public:
    /* Ordered vector of chunk bounds, index == chunk ordinal. */
    std::vector<time_chunk_bounds_t> chunks;

    RDB_DECLARE_ME_SERIALIZABLE(time_chunk_index_t);

    /* Returns indices of chunks overlapping [start_us, end_us). Half-open
     * bounds: a chunk [min, max) overlaps iff min < end_us && max > start_us.
     * Empty ranges (start_us >= end_us) return an empty vector. */
    std::vector<size_t> overlapping_chunks(uint64_t start_us,
                                           uint64_t end_us) const;
};

RDB_DECLARE_EQUALITY_COMPARABLE(time_chunk_bounds_t);
RDB_DECLARE_EQUALITY_COMPARABLE(time_chunk_index_t);

}  // namespace ql

#endif  // BTREE_TIME_CHUNK_HPP_
