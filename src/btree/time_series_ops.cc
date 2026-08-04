// Copyright 2026 RethinkDB, all rights reserved.
#include "btree/time_series_ops.hpp"

#include <string.h>

#include <string>
#include <utility>
#include <vector>

#include "buffer_cache/alt.hpp"
#include "buffer_cache/blob.hpp"
#include "buffer_cache/serialize_onto_blob.hpp"
#include "containers/archive/buffer_group_stream.hpp"
#include "containers/archive/versioned.hpp"
#include "errors.hpp"
#include "rdb_protocol/btree.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/pseudo_time.hpp"
#include "rdb_protocol/time_series_errors.hpp"

/* time_series_catalog_t serialization (LATEST_DISK, like partition_catalog_t
 * in reql_specific.cc: the struct lives in the global namespace). */
RDB_IMPL_SERIALIZABLE_3_SINCE_v2_4(time_series_catalog_t,
    format_version, config, chunk_index);

RDB_IMPL_EQUALITY_COMPARABLE_3(time_series_catalog_t,
    format_version, config, chunk_index);

namespace {

/* Child block under the primary superblock holding the catalog blob.
 * Mirrors partition_ops_t::allocate_catalog_block. */
buf_lock_t allocate_catalog_block(real_superblock_t *sb) {
    buf_lock_t block(sb->expose_buf(), alt_create_t::create);
    {
        buf_write_t write(&block);
        char *ref_slot = static_cast<char *>(
            write.get_data_write(blob::btree_maxreflen));
        memset(ref_slot, 0, blob::btree_maxreflen);
    }
    return block;
}

}  // namespace

time_series_catalog_t time_series_ops_t::load_catalog(real_superblock_t *sb) {
    guarantee(sb != nullptr);

    time_series_catalog_t catalog;
    block_id_t block_id = sb->get_time_series_catalog_block_id();
    if (block_id == NULL_BLOCK_ID) {
        return catalog;
    }

    buf_lock_t catalog_block(sb->expose_buf(), block_id, access_t::read);

    /* Copy the blob ref out of the first btree_maxreflen bytes. blob_t may
     * mutate the ref buffer in place, so we work from a mutable copy. */
    std::vector<char> ref_buf(blob::btree_maxreflen, 0);
    {
        buf_read_t ref_read(&catalog_block);
        uint32_t block_size;
        const char *ref_slot = static_cast<const char *>(
            ref_read.get_data_read(&block_size));
        guarantee(block_size >= static_cast<uint32_t>(blob::btree_maxreflen));
        memcpy(ref_buf.data(), ref_slot, blob::btree_maxreflen);
    }

    blob_t blob(sb->expose_buf().cache()->max_block_size(),
                ref_buf.data(),
                blob::btree_maxreflen);
    {
        buffer_group_t buffer_group;
        blob_acq_t acq;
        blob.expose_all(sb->expose_buf(), access_t::read, &buffer_group, &acq);
        /* The catalog is new in Phase 3; always written as LATEST_DISK. */
        deserialize_from_group<cluster_version_t::LATEST_DISK>(
            const_view(&buffer_group), &catalog);
    }
    return catalog;
}

void time_series_ops_t::save_catalog(
        real_superblock_t *sb, const time_series_catalog_t &catalog) {
    guarantee(sb != nullptr);

    block_id_t block_id = sb->get_time_series_catalog_block_id();
    buf_lock_t catalog_block;
    if (block_id == NULL_BLOCK_ID) {
        catalog_block = allocate_catalog_block(sb);
        block_id = catalog_block.block_id();
        sb->set_time_series_catalog_block_id(block_id);
    } else {
        catalog_block = buf_lock_t(sb->expose_buf(), block_id, access_t::write);
    }

    /* Heap-allocated ref buffer: blob_t reads the ref at construction, then
     * writes the updated ref after serialize_onto_blob (which clears +
     * rewrites). */
    std::vector<char> ref_buf(blob::btree_maxreflen, 0);
    {
        buf_read_t ref_read(&catalog_block);
        uint32_t block_size;
        const char *ref_slot = static_cast<const char *>(
            ref_read.get_data_read(&block_size));
        guarantee(block_size >= static_cast<uint32_t>(blob::btree_maxreflen));
        memcpy(ref_buf.data(), ref_slot, blob::btree_maxreflen);
    }

    {
        blob_t blob(sb->expose_buf().cache()->max_block_size(),
                    ref_buf.data(),
                    blob::btree_maxreflen);
        serialize_onto_blob<cluster_version_t::LATEST_DISK>(
            sb->expose_buf(), &blob, catalog);
    }

    {
        buf_write_t ref_write(&catalog_block);
        char *ref_slot = static_cast<char *>(
            ref_write.get_data_write(blob::btree_maxreflen));
        memcpy(ref_slot, ref_buf.data(), blob::btree_maxreflen);
    }
}

void time_series_ops_t::release_catalog(real_superblock_t *sb) {
    guarantee(sb != nullptr);

    block_id_t block_id = sb->get_time_series_catalog_block_id();
    if (block_id == NULL_BLOCK_ID) {
        return;
    }

    buf_lock_t catalog_block(sb->expose_buf(), block_id, access_t::write);

    /* Clear the blob tree (deallocates overflow sub-blocks), then free the
     * root catalog block itself. */
    std::vector<char> ref_buf(blob::btree_maxreflen, 0);
    {
        buf_read_t ref_read(&catalog_block);
        uint32_t block_size;
        const char *ref_slot = static_cast<const char *>(
            ref_read.get_data_read(&block_size));
        guarantee(block_size >= static_cast<uint32_t>(blob::btree_maxreflen));
        memcpy(ref_buf.data(), ref_slot, blob::btree_maxreflen);
    }
    {
        blob_t blob(sb->expose_buf().cache()->max_block_size(),
                    ref_buf.data(),
                    blob::btree_maxreflen);
        blob.clear(sb->expose_buf());
    }

    catalog_block.write_acq_signal()->wait_lazily_unordered();
    catalog_block.mark_deleted();
    sb->set_time_series_catalog_block_id(NULL_BLOCK_ID);
}

uint64_t time_series_ops_t::extract_time_us(const ql::datum_t &doc,
                                            const std::string &field) {
    /* Non-object documents structurally have no time field. */
    if (doc.get_type() != ql::datum_t::R_OBJECT) {
        time_series_error::raise_op_failed(
            time_series_error::TIME_SERIES_FIELD_MISSING,
            "document is missing time-series field `%s`.",
            field.c_str());
    }

    ql::datum_t ts =
        doc.get_field(datum_string_t(field), ql::NOTHROW);
    if (!ts.has()) {
        time_series_error::raise_op_failed(
            time_series_error::TIME_SERIES_FIELD_MISSING,
            "document is missing time-series field `%s`.",
            field.c_str());
    }
    if (!ts.is_ptype(ql::pseudo::time_string)) {
        time_series_error::raise_op_failed(
            time_series_error::TIME_SERIES_FIELD_INVALID_TYPE,
            "time-series field `%s` must be a ReQL time value "
            "(pseudo-type TIME), got %s.",
            field.c_str(), ts.get_type_name().c_str());
    }

    double epoch_seconds = ql::pseudo::time_to_epoch_time(ts);
    /* Pre-epoch times are clamped to 0 so the uint64 micros domain stays
     * well-ordered; real time-series data is post-1970. */
    if (epoch_seconds < 0) {
        epoch_seconds = 0;
    }
    return static_cast<uint64_t>(epoch_seconds * 1e6);
}

size_t time_series_ops_t::select_chunk(
        ql::time_chunk_index_t *chunk_index,
        const ql::time_series_config_t &config,
        uint64_t chunk_target_rows,
        uint64_t ts_us) {
    guarantee(chunk_index != nullptr);
    std::vector<ql::time_chunk_bounds_t> &chunks = chunk_index->chunks;

    if (chunks.empty()) {
        /* First row: create chunk 0 spanning the single point. */
        ql::time_chunk_bounds_t c;
        c.min_time_us = ts_us;
        c.max_time_us = ts_us + 1;
        c.row_count = 0;
        c.root_block = NULL_BLOCK_ID;
        chunks.push_back(c);
        return 0;
    }

    ql::time_chunk_bounds_t &newest = chunks.back();

    if (ts_us >= newest.max_time_us) {
        /* Append path (§4.1 step 2). Seal when the current chunk is full
         * (row-count threshold, §4.1 step 4) or when the row falls outside
         * the chunk's time interval. */
        const uint64_t interval_us =
            config.chunk_interval_seconds * 1000000ULL;
        const bool over_rows =
            newest.row_count >= chunk_target_rows;
        const bool over_interval =
            ts_us >= newest.min_time_us + interval_us;
        if (over_rows || over_interval) {
            /* Seal the current chunk. On an interval seal, extend its max
             * to the new chunk's start so the chunks tile the time axis
             * contiguously (spec §5.1) — otherwise rows between the last
             * row and the boundary fall into a gap. */
            if (over_interval) {
                newest.max_time_us = ts_us;
            }
            ql::time_chunk_bounds_t c;
            c.min_time_us = ts_us;
            c.max_time_us = ts_us + 1;
            c.row_count = 0;
            c.root_block = NULL_BLOCK_ID;
            chunks.push_back(c);
            return chunks.size() - 1;
        }
        /* Extend the newest chunk's exclusive upper bound. */
        newest.max_time_us = ts_us + 1;
        return chunks.size() - 1;
    }

    /* Backfill / out-of-order (§4.1 step 3): find the containing chunk.
     * The index is ordered, but chunk counts are small and linear scan
     * keeps the logic obviously correct; a binary search is a later-phase
     * optimization. */
    for (size_t i = 0; i < chunks.size(); ++i) {
        if (chunks[i].min_time_us <= ts_us && ts_us < chunks[i].max_time_us) {
            return i;
        }
    }

    /* Older than every chunk: prepend a new chunk. */
    ql::time_chunk_bounds_t c;
    c.min_time_us = ts_us;
    c.max_time_us = ts_us + 1;
    c.row_count = 0;
    c.root_block = NULL_BLOCK_ID;
    chunks.insert(chunks.begin(), c);
    return 0;
}

uint64_t time_series_ops_t::total_rows(
        const ql::time_chunk_index_t &chunk_index) {
    uint64_t total = 0;
    for (const ql::time_chunk_bounds_t &c : chunk_index.chunks) {
        total += c.row_count;
    }
    return total;
}

void time_series_ops_t::route_insert(
        btree_slice_t *slice,
        real_superblock_t *sb,
        const ql::time_series_config_t &config,
        time_series_catalog_t *catalog,
        uint64_t chunk_target_rows,
        const store_key_t &key,
        const ql::datum_t &data,
        bool overwrite,
        repli_timestamp_t timestamp,
        const deletion_context_t *deletion_context,
        point_write_response_t *response_out,
        rdb_modification_info_t *mod_info,
        profile::trace_t *trace) {
    guarantee(slice != nullptr);
    guarantee(sb != nullptr);
    guarantee(catalog != nullptr);
    guarantee(deletion_context != nullptr);
    guarantee(response_out != nullptr);
    guarantee(mod_info != nullptr);

    const uint64_t ts_us =
        extract_time_us(data, config.time_field.str());

    const size_t chunk_idx = select_chunk(
        &catalog->chunk_index, config, chunk_target_rows, ts_us);
    ql::time_chunk_bounds_t &chunk = catalog->chunk_index.chunks[chunk_idx];

    /* Route the write into the chunk's own B-tree. */
    time_chunk_superblock_t chunk_sb(
        sb, chunk.root_block, sb->get_stat_block_id());
    rdb_set(key, data, overwrite, slice, timestamp, &chunk_sb,
            deletion_context, response_out, mod_info, trace);
    chunk.root_block = chunk_sb.get_root_block_id();
    /* row_count tracks distinct rows in the chunk: an overwrite (the key
     * already existed, so `deleted` carries the old value) does not grow
     * the chunk. */
    if (!mod_info->deleted.first.has()) {
        chunk.row_count += 1;
    }
}
