// Copyright 2026 RethinkDB, all rights reserved.
#include "btree/time_chunk.hpp"

#include <cstddef>
#include <utility>
#include <vector>

#include "containers/archive/stl_types.hpp"

namespace ql {

std::vector<size_t> time_chunk_index_t::overlapping_chunks(
        uint64_t start_us, uint64_t end_us) const {
    std::vector<size_t> out;
    if (start_us >= end_us) {
        return out;
    }
    for (size_t i = 0; i < chunks.size(); ++i) {
        if (chunks[i].min_time_us < end_us && chunks[i].max_time_us > start_us) {
            out.push_back(i);
        }
    }
    return out;
}

RDB_IMPL_SERIALIZABLE_4_SINCE_v2_4(time_chunk_bounds_t,
    min_time_us, max_time_us, row_count, root_block);

RDB_IMPL_SERIALIZABLE_1_SINCE_v2_4(time_chunk_index_t, chunks);

RDB_IMPL_EQUALITY_COMPARABLE_4(time_chunk_bounds_t,
    min_time_us, max_time_us, row_count, root_block);

RDB_IMPL_EQUALITY_COMPARABLE_1(time_chunk_index_t, chunks);

}  // namespace ql
