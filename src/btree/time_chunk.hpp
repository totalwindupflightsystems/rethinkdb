// Copyright 2026 RethinkDB, all rights reserved.
#ifndef BTREE_TIME_CHUNK_HPP_
#define BTREE_TIME_CHUNK_HPP_

#include <cstdint>
#include <vector>

#include "serializer/types.hpp"
#include "rpc/semilattice/joins/macros.hpp"
#include "rpc/serialize_macros.hpp"

/* Time-chunk metadata (Phase 3 spec §3.2, extended by PHASE3-TS-2 §5.1).
 *
 * Each time-series chunk is a self-contained sub-B-tree within the table's
 * storage; the chunk index is the BRIN-like sparse index used for read
 * pruning and retention. Bounds are half-open: [min_time_us, max_time_us).
 *
 * PHASE3-TS-2 added `root_block`: the root block id of the chunk's own
 * B-tree (NULL_BLOCK_ID until the chunk's first row is written). This is
 * what makes the chunk index addressable as a set of independent trees
 * instead of a pure statistics summary. Serialization is SINCE_v2_4 like
 * the rest of the fork's Phase-3 catalog data (no pre-release on-disk
 * compat concern).
 *
 * PHASE3-TS-5 added `merged`: the downsample merge watermark. A sealed
 * chunk whose rows have been folded into every downsample step's tree is
 * marked merged so the background job skips it on later passes (idempotent
 * re-runs, spec §6.4). The flag is durable with the chunk index (catalog
 * save commits atomically with the merge writes), so a crash between
 * chunks rolls the in-flight chunk back and the next pass resumes from the
 * flag. The flag is per-chunk rather than a scalar watermark because the
 * chunk index can shift (retention erases the front, out-of-order writes
 * prepend): a scalar "merged through time" watermark would silently skip
 * a prepended chunk. Rows backfilled into an already-merged sealed chunk
 * are not re-merged (documented approximation; sealed chunk bounds are
 * stable, so this only affects out-of-order writes that land inside a
 * merged chunk's interval). */

namespace ql {

struct time_chunk_bounds_t {
    uint64_t min_time_us;  // micros since epoch
    uint64_t max_time_us;  // exclusive upper bound
    uint64_t row_count;
    block_id_t root_block;  // root of this chunk's sub-B-tree, NULL if empty
    bool merged;            // PHASE3-TS-5: folded into all downsample trees

    time_chunk_bounds_t()
        : min_time_us(0), max_time_us(0), row_count(0),
          root_block(NULL_BLOCK_ID), merged(false) { }

    time_chunk_bounds_t(uint64_t min_us, uint64_t max_us, uint64_t rows,
                        block_id_t root = NULL_BLOCK_ID)
        : min_time_us(min_us), max_time_us(max_us), row_count(rows),
          root_block(root), merged(false) { }

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
