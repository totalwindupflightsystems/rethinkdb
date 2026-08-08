// Copyright 2026 RethinkDB, all rights reserved.
#include "btree/time_series_ops.hpp"

#include <string.h>

#include <algorithm>
#include <cinttypes>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "btree/depth_first_traversal.hpp"
#include "buffer_cache/alt.hpp"
#include "buffer_cache/blob.hpp"
#include "buffer_cache/serialize_onto_blob.hpp"
#include "containers/archive/buffer_group_stream.hpp"
#include "containers/archive/versioned.hpp"
#include "errors.hpp"
#include "rdb_protocol/btree.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/erase_range.hpp"
#include "rdb_protocol/lazy_btree_val.hpp"
#include "rdb_protocol/pseudo_time.hpp"
#include "rdb_protocol/time_series_errors.hpp"
#include "utils.hpp"

/* time_series_catalog_t serialization (LATEST_DISK, like partition_catalog_t
 * in reql_specific.cc: the struct lives in the global namespace). The
 * downsample roots are appended AFTER chunk_index (PHASE3-TS-5 §5.3) so the
 * pre-existing fields stay position-stable. */
RDB_IMPL_SERIALIZABLE_4_SINCE_v2_4(time_series_catalog_t,
    format_version, config, chunk_index, downsample_roots);

RDB_IMPL_SERIALIZABLE_2_SINCE_v2_4(downsample_root_t,
    target_interval_seconds, root_block);

RDB_IMPL_EQUALITY_COMPARABLE_4(time_series_catalog_t,
    format_version, config, chunk_index, downsample_roots);

RDB_IMPL_EQUALITY_COMPARABLE_2(downsample_root_t,
    target_interval_seconds, root_block);

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

/* ── retention + compaction (PHASE3-TS-4, spec §5.2/§6.4/§7) ──────────── */

std::vector<size_t> time_series_ops_t::expired_chunks(
        const ql::time_chunk_index_t &chunk_index,
        uint64_t now_us, uint64_t retention_seconds) {
    std::vector<size_t> expired;
    if (retention_seconds == 0) {
        /* retention = 0 disables retention (§5.2). */
        return expired;
    }
    const uint64_t retention_us = retention_seconds * 1000000ULL;
    /* Clock-underflow guard: before the retention span has elapsed the
     * cutoff is clamped to 0, and no chunk (max_time_us >= 1 always) can
     * be strictly below it. */
    const uint64_t cutoff_us =
        (now_us >= retention_us) ? now_us - retention_us : 0;
    /* Chunks tile the time axis in order, so expired chunks form a prefix
     * of the index; stop at the first chunk whose newest row is at or
     * after the cutoff (exactly-at-TTL kept, §8.1). */
    for (size_t i = 0; i < chunk_index.chunks.size(); ++i) {
        if (chunk_index.chunks[i].max_time_us < cutoff_us) {
            expired.push_back(i);
        } else {
            break;
        }
    }
    return expired;
}

std::vector<size_t> time_series_ops_t::compactible_chunks(
        const ql::time_chunk_index_t &chunk_index,
        uint64_t now_us, uint64_t min_age_seconds) {
    std::vector<size_t> compactible;
    const uint64_t min_age_us = min_age_seconds * 1000000ULL;
    const uint64_t cutoff_us =
        (now_us >= min_age_us) ? now_us - min_age_us : 0;
    /* The newest chunk is the active write target and is never compacted
     * (§6.4: "chunk sealed + 1 hour old"). */
    for (size_t i = 0; i + 1 < chunk_index.chunks.size(); ++i) {
        const ql::time_chunk_bounds_t &chunk = chunk_index.chunks[i];
        if (chunk.row_count > 0 && chunk.max_time_us <= cutoff_us) {
            compactible.push_back(i);
        }
    }
    return compactible;
}

bool time_series_ops_t::chunk_is_corrupt(const ql::time_chunk_bounds_t &chunk) {
    /* §7: the index claims rows but no tree root exists to hold them. */
    return chunk.row_count > 0 && chunk.root_block == NULL_BLOCK_ID;
}

namespace {

/* Frees one chunk's entire tree in bounded batches (§9.3 memory bound:
 * O(TIME_SERIES_RETENTION_ERASE_BATCH) per pass). Appends every deleted
 * row's modification report to `mod_reports_out` (or discards them when
 * null) and returns the number of rows freed. */
uint64_t erase_chunk_tree(
        btree_slice_t *slice,
        real_superblock_t *sb,
        block_id_t chunk_root,
        const deletion_context_t *deletion_context,
        signal_t *interruptor,
        std::vector<rdb_modification_report_t> *mod_reports_out) {
    always_true_key_tester_t key_tester;
    /* The adapter routes the erase into the chunk's own tree and tracks
     * the shrinking root across passes. */
    time_chunk_superblock_t chunk_sb(
        sb, chunk_root, sb->get_stat_block_id());
    uint64_t erased = 0;
    for (continue_bool_t done = continue_bool_t::CONTINUE;
         done == continue_bool_t::CONTINUE;) {
        std::vector<rdb_modification_report_t> pass_reports;
        key_range_t deleted_range;
        done = rdb_erase_small_range(
            slice, &key_tester, key_range_t::universe(), &chunk_sb,
            deletion_context, interruptor, TIME_SERIES_RETENTION_ERASE_BATCH,
            &pass_reports, &deleted_range);
        erased += pass_reports.size();
        if (mod_reports_out != nullptr) {
            mod_reports_out->insert(mod_reports_out->end(),
                                    pass_reports.begin(),
                                    pass_reports.end());
        }
        /* rdb_erase_small_range clears and refills pass_reports every
         * call; keep its memory bounded by draining after each pass. */
        pass_reports.clear();
        pass_reports.shrink_to_fit();
    }
    return erased;
}

/* Collects every (key, datum) pair of a chunk tree for the compaction
 * rewrite. Traversal order is lexicographic; the rebuild sorts anyway. */
class chunk_collect_visitor_t : public depth_first_traversal_callback_t {
public:
    continue_bool_t handle_pair(
            scoped_key_value_t &&keyvalue,
            UNUSED signal_t *interruptor) override {
        const store_key_t key(keyvalue.key());
        const rdb_value_t *value =
            static_cast<const rdb_value_t *>(keyvalue.value());
        rows.emplace_back(key, get_data(value, keyvalue.expose_buf()));
        return continue_bool_t::CONTINUE;
    }

    std::vector<std::pair<store_key_t, ql::datum_t>> rows;
};

}  // namespace

uint64_t time_series_ops_t::expire_chunk(
        btree_slice_t *slice,
        real_superblock_t *sb,
        time_series_catalog_t *catalog,
        size_t chunk_idx,
        const deletion_context_t *deletion_context,
        signal_t *interruptor,
        std::vector<rdb_modification_report_t> *mod_reports_out) {
    guarantee(slice != nullptr);
    guarantee(sb != nullptr);
    guarantee(catalog != nullptr);
    guarantee(deletion_context != nullptr);
    guarantee(mod_reports_out != nullptr);
    guarantee(chunk_idx < catalog->chunk_index.chunks.size());

    const ql::time_chunk_bounds_t &chunk =
        catalog->chunk_index.chunks[chunk_idx];
    if (chunk_is_corrupt(chunk)) {
        /* §7: never erase anything around a corrupt chunk. This raises
         * BEFORE any modification, so the caller's transaction is
         * untouched. */
        time_series_error::raise_op_failed(
            time_series_error::TIME_SERIES_CHUNK_CORRUPT,
            "cannot expire chunk %zu: the chunk index claims %" PRIu64
            " rows but no tree root exists.",
            chunk_idx, chunk.row_count);
    }

    uint64_t rows_freed = 0;
    if (chunk.root_block != NULL_BLOCK_ID) {
        rows_freed = erase_chunk_tree(
            slice, sb, chunk.root_block, deletion_context, interruptor,
            mod_reports_out);
    }
    /* The index is the checkpoint: the caller saves the catalog in the
     * same transaction as the erase, so a crash rolls the whole chunk
     * back and the next pass resumes from the front. */
    catalog->chunk_index.chunks.erase(
        catalog->chunk_index.chunks.begin() + chunk_idx);
    return rows_freed;
}

uint64_t time_series_ops_t::compact_chunk(
        btree_slice_t *slice,
        real_superblock_t *sb,
        time_series_catalog_t *catalog,
        size_t chunk_idx,
        const deletion_context_t *deletion_context,
        signal_t *interruptor) {
    guarantee(slice != nullptr);
    guarantee(sb != nullptr);
    guarantee(catalog != nullptr);
    guarantee(deletion_context != nullptr);
    guarantee(chunk_idx < catalog->chunk_index.chunks.size());

    const ql::time_chunk_bounds_t &chunk =
        catalog->chunk_index.chunks[chunk_idx];
    if (chunk_is_corrupt(chunk)) {
        time_series_error::raise_op_failed(
            time_series_error::TIME_SERIES_CHUNK_CORRUPT,
            "cannot compact chunk %zu: the chunk index claims %" PRIu64
            " rows but no tree root exists.",
            chunk_idx, chunk.row_count);
    }
    if (chunk.root_block == NULL_BLOCK_ID) {
        /* Empty chunk: nothing to rewrite (compactible_chunks filters
         * these, but be defensive). */
        return 0;
    }

    /* 1. Collect the chunk's rows. Memory is bounded by one chunk's data
     * (§9.3). */
    chunk_collect_visitor_t collector;
    {
        time_chunk_superblock_t read_sb(
            sb, chunk.root_block, sb->get_stat_block_id());
        btree_depth_first_traversal(
            &read_sb, key_range_t::universe(), &collector,
            access_t::read, direction_t::FORWARD,
            release_superblock_t::KEEP, interruptor);
    }

    /* 2. Rebuild a fresh packed tree. Inserting in ascending key order
     * gives left-packed leaves; the new root is allocated BEFORE the old
     * tree is freed, so the published root is deterministically a new
     * block (never a recycled old one). */
    std::sort(collector.rows.begin(), collector.rows.end(),
              [](const std::pair<store_key_t, ql::datum_t> &a,
                 const std::pair<store_key_t, ql::datum_t> &b) {
                  return a.first < b.first;
              });
    time_chunk_superblock_t new_sb(
        sb, NULL_BLOCK_ID, sb->get_stat_block_id());
    for (const std::pair<store_key_t, ql::datum_t> &row : collector.rows) {
        point_write_response_t pw_res;
        rdb_modification_info_t mod_info;
        rdb_set(row.first, row.second, true /* overwrite */, slice,
                repli_timestamp_t::distant_past, &new_sb,
                deletion_context, &pw_res, &mod_info, nullptr);
    }

    /* 3. Free the old tree's blocks. The values were re-serialized into
     * the new tree (fresh blob refs), so the old blobs are reclaimed like
     * any row delete: the in-tree deleter detaches them during the erase,
     * then the post-deleter frees the blocks. Secondary indexes are NOT
     * touched — the row values are unchanged, so their entries stay
     * valid (unlike retention, which feeds reports to update_sindexes). */
    std::vector<rdb_modification_report_t> old_reports;
    erase_chunk_tree(slice, sb, chunk.root_block, deletion_context,
                     interruptor, &old_reports);
    for (const rdb_modification_report_t &report : old_reports) {
        if (report.info.deleted.first.has()) {
            deletion_context->post_deleter()->delete_value(
                sb->expose_buf(), report.info.deleted.second.data());
        }
    }

    /* 4. Publish the new root (row_count unchanged). */
    catalog->chunk_index.chunks[chunk_idx].root_block =
        new_sb.get_root_block_id();
    return collector.rows.size();
}

/* ── downsampling (PHASE3-TS-5, spec §4.3/§5.3/§6.1/§6.4) ──────────────── */

std::vector<size_t> time_series_ops_t::downsample_candidates(
        const ql::time_chunk_index_t &chunk_index) {
    std::vector<size_t> candidates;
    /* The newest chunk is the active write target and is never merged
     * (same rule as compaction, §6.4). Sealed chunks that already carry the
     * merged watermark are skipped — the watermark IS the idempotency
     * checkpoint (foundation commit). */
    for (size_t i = 0; i + 1 < chunk_index.chunks.size(); ++i) {
        const ql::time_chunk_bounds_t &chunk = chunk_index.chunks[i];
        if (chunk.row_count > 0 && !chunk.merged) {
            candidates.push_back(i);
        }
    }
    return candidates;
}

std::vector<std::pair<store_key_t, ql::datum_t>>
time_series_ops_t::collect_chunk_rows(
        btree_slice_t *slice, real_superblock_t *sb,
        const time_series_catalog_t *catalog, size_t chunk_idx,
        signal_t *interruptor) {
    guarantee(slice != nullptr);
    guarantee(sb != nullptr);
    guarantee(catalog != nullptr);
    guarantee(interruptor != nullptr);
    guarantee(chunk_idx < catalog->chunk_index.chunks.size());

    const ql::time_chunk_bounds_t &chunk =
        catalog->chunk_index.chunks[chunk_idx];
    if (chunk_is_corrupt(chunk)) {
        /* §7: never read around a corrupt chunk; raises BEFORE any
         * traversal, so the caller's transaction is untouched. */
        time_series_error::raise_op_failed(
            time_series_error::TIME_SERIES_CHUNK_CORRUPT,
            "cannot downsample chunk %zu: the chunk index claims %" PRIu64
            " rows but no tree root exists.",
            chunk_idx, chunk.row_count);
    }

    chunk_collect_visitor_t collector;
    if (chunk.root_block != NULL_BLOCK_ID) {
        time_chunk_superblock_t read_sb(
            sb, chunk.root_block, sb->get_stat_block_id());
        btree_depth_first_traversal(
            &read_sb, key_range_t::universe(), &collector,
            access_t::read, direction_t::FORWARD,
            release_superblock_t::KEEP, interruptor);
    }
    return std::move(collector.rows);
}

void time_series_ops_t::write_downsample_rows(
        btree_slice_t *slice, real_superblock_t *sb,
        const std::vector<std::pair<store_key_t, ql::datum_t>> &rows,
        block_id_t *root_in_out, const deletion_context_t *deletion_context) {
    guarantee(slice != nullptr);
    guarantee(sb != nullptr);
    guarantee(root_in_out != nullptr);
    guarantee(deletion_context != nullptr);
    if (rows.empty()) {
        return;
    }

    /* Ascending key order gives append locality in the rightmost leaf (the
     * same trick as the compaction rebuild). */
    std::vector<std::pair<store_key_t, ql::datum_t>> sorted = rows;
    std::sort(sorted.begin(), sorted.end(),
              [](const std::pair<store_key_t, ql::datum_t> &a,
                 const std::pair<store_key_t, ql::datum_t> &b) {
                  return a.first < b.first;
              });

    time_chunk_superblock_t ds_sb(
        sb, *root_in_out, sb->get_stat_block_id());
    for (const std::pair<store_key_t, ql::datum_t> &row : sorted) {
        point_write_response_t pw_res;
        rdb_modification_info_t mod_info;
        rdb_set(row.first, row.second, true /* overwrite */, slice,
                repli_timestamp_t::distant_past, &ds_sb,
                deletion_context, &pw_res, &mod_info, nullptr);
    }
    *root_in_out = ds_sb.get_root_block_id();
}

uint64_t time_series_ops_t::merge_chunk_into_downsample_steps(
        btree_slice_t *slice, real_superblock_t *sb,
        time_series_catalog_t *catalog, size_t chunk_idx,
        const deletion_context_t *deletion_context, signal_t *interruptor) {
    guarantee(slice != nullptr);
    guarantee(sb != nullptr);
    guarantee(catalog != nullptr);
    guarantee(deletion_context != nullptr);
    guarantee(interruptor != nullptr);
    guarantee(chunk_idx < catalog->chunk_index.chunks.size());

    const ql::time_chunk_bounds_t &chunk =
        catalog->chunk_index.chunks[chunk_idx];
    if (chunk_is_corrupt(chunk)) {
        /* §7: never merge around a corrupt chunk; raises BEFORE any
         * modification, so the caller's transaction is untouched. */
        time_series_error::raise_op_failed(
            time_series_error::TIME_SERIES_CHUNK_CORRUPT,
            "cannot downsample chunk %zu: the chunk index claims %" PRIu64
            " rows but no tree root exists.",
            chunk_idx, chunk.row_count);
    }

    /* Keep `downsample_roots` parallel to the (immutable) step list:
     * index i holds step i's tree root. A catalog that predates the
     * first merge gets a NULL root per step; the first fold creates the
     * tree. */
    while (catalog->downsample_roots.size()
            < catalog->config.downsample_steps.size()) {
        catalog->downsample_roots.push_back(downsample_root_t(
            catalog->config.downsample_steps[
                catalog->downsample_roots.size()].target_interval_seconds));
    }

    /* One bounded unit per transaction: one chunk's rows in memory (§9.3).
     * The fold into ALL steps plus the watermark commit as one transaction
     * in the caller, so a crash rolls the in-flight chunk back entirely
     * (its step writes were never committed) and the next pass resumes
     * from the flag — no duplicate buckets, no double counting. */
    uint64_t rows_written = 0;

    std::vector<std::pair<store_key_t, ql::datum_t>> rows =
        collect_chunk_rows(slice, sb, catalog, chunk_idx, interruptor);

    /* Aggregate evaluation env (same construction as the BRIN read path).
     * Aggregate funcs were validated deterministic at table creation. */
    ql::env_t agg_env(interruptor,
                      ql::return_empty_normal_batches_t::NO,
                      reql_version_t::LATEST);

    for (size_t step_idx = 0;
         step_idx < catalog->config.downsample_steps.size();
         ++step_idx) {
        const ql::downsample_step_t &step =
            catalog->config.downsample_steps[step_idx];
        const uint64_t interval_us =
            step.target_interval_seconds * 1000000ULL;

        /* Compile the step's aggregate funcs ONCE (per step, not per
         * bucket — the generated-columns compile-once precedent). Each
         * func is a 1-arg function over the row batch (the batch is bound
         * to r.row; e.g. `r.avg(r.row("temperature"))`). */
        std::map<name_string_t, counted_t<const ql::func_t>> funcs;
        for (const auto &kv : step.aggregates) {
            funcs[kv.first] = kv.second.compile_wire_func();
        }

        /* Bucket the chunk's rows: bucket = floor(ts / interval). */
        std::map<uint64_t, std::vector<ql::datum_t>> buckets;
        for (const auto &row : rows) {
            const uint64_t ts_us = extract_time_us(
                row.second, catalog->config.time_field.str());
            buckets[(ts_us / interval_us) * interval_us].push_back(
                row.second);
        }

        /* ONE downsample row per bucket per step, keyed by the bucket
         * start: the aggregate columns computed over the bucket's rows.
         * The bucket-start key (20-digit zero-padded micros string)
         * doubles as the bucket's start time, so the planner's key-range
         * bound is exact and chronological. */
        std::vector<std::pair<store_key_t, ql::datum_t>> out_rows;
        for (const auto &bucket : buckets) {
            ql::datum_t batch = ql::datum_t(
                std::vector<ql::datum_t>(bucket.second),
                ql::configured_limits_t::unlimited);
            ql::datum_object_builder_t agg_row;
            for (const auto &f : funcs) {
                ql::datum_t val = f.second->call(
                    &agg_env, std::vector<ql::datum_t>{batch})
                        ->as_datum();
                agg_row.overwrite(f.first.str().c_str(), val);
            }
            out_rows.emplace_back(
                downsample_bucket_key(bucket.first),
                std::move(agg_row).to_datum());
        }
        rows_written += out_rows.size();

        write_downsample_rows(
            slice, sb, out_rows,
            &catalog->downsample_roots[step_idx].root_block,
            deletion_context);
    }

    /* The chunk is folded into EVERY step: set the durable merge
     * watermark. The caller's catalog save commits atomically with the
     * step writes in this same transaction. */
    catalog->chunk_index.chunks[chunk_idx].merged = true;
    return rows_written;
}

store_key_t time_series_ops_t::downsample_bucket_key(uint64_t bucket_start_us) {
    /* 20 digits = the width of UINT64_MAX ("18446744073709551615"): the
     * zero padding is what makes string order equal numeric order, which
     * the range bound in `downsample_key_range` relies on. */
    return store_key_t(strprintf("%020" PRIu64, bucket_start_us));
}

key_range_t time_series_ops_t::downsample_key_range(
        uint64_t start_us, uint64_t end_us, uint64_t interval_seconds) {
    if (start_us >= end_us) {
        return key_range_t::empty();
    }
    const uint64_t interval_us = interval_seconds * 1000000ULL;
    const uint64_t first_bucket_us = (start_us / interval_us) * interval_us;
    const store_key_t left = downsample_bucket_key(first_bucket_us);
    if (end_us == std::numeric_limits<uint64_t>::max()) {
        /* Unbounded right edge (open-ended between): every bucket at or
         * after `first_bucket_us`. */
        return key_range_t(key_range_t::closed, left,
                           key_range_t::none, store_key_t());
    }
    return key_range_t(key_range_t::closed, left,
                       key_range_t::open, downsample_bucket_key(end_us));
}

const downsample_root_t *time_series_ops_t::select_downsample_root(
        const time_series_catalog_t &catalog,
        uint64_t start_us, uint64_t end_us) {
    if (start_us >= end_us) {
        return nullptr;
    }
    /* Spec §4.3: range_seconds = window / 1e6; select_downsample applies
     * the strictly-less-than age semantics (reused verbatim). */
    const uint64_t range_seconds = (end_us - start_us) / 1000000ULL;
    const ql::downsample_step_t *step =
        catalog.config.select_downsample(range_seconds);
    if (step == nullptr) {
        return nullptr;
    }
    /* downsample_roots is parallel to config.downsample_steps; the step
     * pointer selects its index directly. */
    const size_t step_idx = step - catalog.config.downsample_steps.data();
    if (step_idx >= catalog.downsample_roots.size()) {
        return nullptr;
    }
    const downsample_root_t *root = &catalog.downsample_roots[step_idx];
    if (root->root_block == NULL_BLOCK_ID) {
        /* The step exists but no merge has written into its tree yet: fall
         * back to the raw chunk-root path (reads must not return empty for
         * a table whose raw data exists). */
        return nullptr;
    }
    return root;
}

void time_series_ops_t::release_storage(
        btree_slice_t *slice, real_superblock_t *sb,
        time_series_catalog_t *catalog,
        const deletion_context_t *deletion_context, signal_t *interruptor,
        std::vector<rdb_modification_report_t> *mod_reports_out) {
    guarantee(slice != nullptr);
    guarantee(sb != nullptr);
    guarantee(catalog != nullptr);
    guarantee(deletion_context != nullptr);
    guarantee(interruptor != nullptr);
    guarantee(mod_reports_out != nullptr);

    /* Erase every chunk tree (bounded batches — expire_chunk's model) and
     * every downsample tree. The caller releases the catalog blob in the
     * same transaction afterwards, so a crash rolls the whole drop back. */
    for (const ql::time_chunk_bounds_t &chunk : catalog->chunk_index.chunks) {
        if (chunk.root_block != NULL_BLOCK_ID) {
            erase_chunk_tree(slice, sb, chunk.root_block, deletion_context,
                             interruptor, mod_reports_out);
        }
    }
    for (const downsample_root_t &root : catalog->downsample_roots) {
        if (root.root_block != NULL_BLOCK_ID) {
            erase_chunk_tree(slice, sb, root.root_block, deletion_context,
                             interruptor, mod_reports_out);
        }
    }
}
