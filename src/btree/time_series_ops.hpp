// Copyright 2026 RethinkDB, all rights reserved.
#ifndef BTREE_TIME_SERIES_OPS_HPP_
#define BTREE_TIME_SERIES_OPS_HPP_

#include <cstdint>
#include <string>

#include "btree/keys.hpp"
#include "btree/operations.hpp"
#include "btree/reql_specific.hpp"
#include "btree/time_chunk.hpp"
#include "btree/time_series_config.hpp"
#include "rpc/serialize_macros.hpp"

/* Time-series storage operations (PHASE3-TS-2, spec §3.3/§4.1/§5.1/§6.2).
 *
 * Mirrors the fork's partition-catalog pattern (see `partition_ops_t`): all
 * durable time-series metadata lives in one blob stored in a child block of
 * the table's primary superblock, addressed via
 * `real_superblock_t::time_series_catalog_block`. The blob holds both the
 * table's time-series config (a durable copy of the Raft-side config so the
 * storage engine is self-sufficient) and the chunk index.
 *
 * Chunk organization (§5.1): every time chunk is a self-contained sub-B-tree
 * rooted at `time_chunk_bounds_t::root_block`. Writes are routed to the
 * chunk's tree through `time_chunk_superblock_t`, a superblock adapter that
 * redirects `get/set_root_block_id` to the chunk root while sharing the
 * table's cache, stat block and write transaction. For monotonically
 * increasing timestamps the active chunk receives only appends, which land
 * in the rightmost leaf of its tree (the standard B-tree already appends
 * in-place to the last leaf for keys arriving in order, so the chunk
 * organization gives the append-optimized write locality of §5.1 without a
 * new node format). */

class txn_t;
class btree_slice_t;
class real_superblock_t;
class deletion_context_t;
class signal_t;
struct point_write_response_t;
struct rdb_modification_info_t;
struct rdb_modification_report_t;

namespace profile {
class trace_t;
}  // namespace profile

/* On-disk format version for time_series_catalog_t. Bump when the serialized
 * layout changes incompatibly. */
static constexpr uint32_t TIME_SERIES_CATALOG_FORMAT_VERSION = 1;

/* Target row count for a chunk. When the newest chunk's row_count reaches
 * this bound the next insert seals it and starts a new chunk (§4.1 step 4).
 * Tuned via this constant; unit tests pass smaller thresholds directly.
 * 1M rows ≈ ~256 MB of leaf data at ~256 bytes/doc, a reasonable balance
 * between chunk-index size and append locality. */
static constexpr uint64_t TIME_SERIES_CHUNK_TARGET_ROWS = 1000000;

/* Erase batch cap for chunk deletion (PHASE3-TS-4, §9.3 memory bound):
 * rdb_erase_small_range uses O(batch) memory, so a chunk tree is freed in
 * bounded passes of at most this many rows. */
static constexpr uint64_t TIME_SERIES_RETENTION_ERASE_BATCH = 1000;

/* PHASE3-TS-5 (§5.3): one downsample tree per downsample step. `root_block`
 * is the root of the step's parallel B-tree (NULL_BLOCK_ID until the first
 * merge writes into it); the step is identified by its
 * `target_interval_seconds` so the entry is self-describing in the catalog
 * blob and debuggable without the config. Rows are keyed by the 20-digit
 * zero-padded decimal string of the bucket start (microseconds), so
 * lexicographic key order equals chronological order. */
struct downsample_root_t {
    uint64_t target_interval_seconds;
    block_id_t root_block;

    downsample_root_t()
        : target_interval_seconds(0), root_block(NULL_BLOCK_ID) { }

    downsample_root_t(uint64_t target_interval, block_id_t root = NULL_BLOCK_ID)
        : target_interval_seconds(target_interval), root_block(root) { }

    RDB_DECLARE_ME_SERIALIZABLE(downsample_root_t);
};

/* Durable time-series catalog blob. Serialized at LATEST_DISK like the
 * partition catalog (Phase-3 data never existed in an older format).
 *
 * PHASE3-TS-5 (§5.3): `downsample_roots` is parallel to
 * `config.downsample_steps` (index i holds the root of step i's downsample
 * tree). The step list is immutable after table creation (only retention is
 * reconfigurable), so the index alignment is stable for the table's life;
 * the merge job appends entries in step order to keep the vectors aligned.
 * Appended AFTER `chunk_index` so the pre-existing fields stay
 * position-stable in the serialized blob. */
struct time_series_catalog_t {
    uint32_t format_version;
    ql::time_series_config_t config;
    ql::time_chunk_index_t chunk_index;
    std::vector<downsample_root_t> downsample_roots;

    time_series_catalog_t()
        : format_version(TIME_SERIES_CATALOG_FORMAT_VERSION) { }

    RDB_DECLARE_ME_SERIALIZABLE(time_series_catalog_t);
};

/* superblock_t adapter that routes B-tree operations into a chunk's own
 * tree. The delegate (the table's real superblock) provides the cache /
 * stat block / write transaction; the root block id is the chunk's. The
 * caller reads back the (possibly updated) root id after the write and
 * persists it into the chunk index. `release()` is a no-op because the
 * delegate is owned by the caller. */
class time_chunk_superblock_t : public superblock_t {
public:
    time_chunk_superblock_t(superblock_t *delegate,
                            block_id_t chunk_root,
                            block_id_t stat_block)
        : delegate_(delegate), chunk_root_(chunk_root), stat_block_(stat_block) { }

    void release() override { }

    block_id_t get_root_block_id() override { return chunk_root_; }
    void set_root_block_id(block_id_t new_root) override {
        chunk_root_ = new_root;
    }
    block_id_t get_stat_block_id() override { return stat_block_; }
    buf_parent_t expose_buf() override { return delegate_->expose_buf(); }

private:
    superblock_t *delegate_;
    block_id_t chunk_root_;
    block_id_t stat_block_;

    DISABLE_COPYING(time_chunk_superblock_t);
};

class time_series_ops_t {
public:
    /* ── catalog blob persistence (mirrors partition_ops_t) ────────────── */

    /* Loads the time-series catalog from the superblock child blob. Returns
     * a default (format_version set, config disabled, empty chunk index)
     * catalog when the block does not exist. */
    static time_series_catalog_t load_catalog(real_superblock_t *sb);

    /* Creates (or overwrites) the catalog blob and points the superblock at
     * it. Must be called inside the write transaction that also performed
     * the chunk-tree modifications, so catalog + data commit atomically. */
    static void save_catalog(real_superblock_t *sb,
                             const time_series_catalog_t &catalog);

    /* Frees the catalog blob and resets the superblock slot to NULL. */
    static void release_catalog(real_superblock_t *sb);

    /* ── write routing (spec §4.1 / §6.2) ─────────────────────────────── */

    /* Extracts the time-field value from a document and converts it to
     * microseconds since epoch. Throws the catalog errors
     * TIME_SERIES_FIELD_MISSING / TIME_SERIES_FIELD_INVALID_TYPE. */
    static uint64_t extract_time_us(const ql::datum_t &doc,
                                    const std::string &field);

    /* Routes one insert into the chunked storage:
     *   1. extract + validate the time field
     *   2. select the destination chunk (create / extend / seal / backfill)
     *   3. rdb_set the row into that chunk's B-tree via the chunk superblock
     *   4. update the chunk bounds + row count in `catalog`
     * The catalog must be saved by the caller after the batch completes.
     * `chunk_target_rows` is the seal threshold (pass
     * TIME_SERIES_CHUNK_TARGET_ROWS from production paths; unit tests pass
     * small values). */
    static void route_insert(btree_slice_t *slice,
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
                             profile::trace_t *trace);

    /* Selects the chunk a row with time `ts_us` belongs to, creating or
     * sealing chunks as needed (§4.1). Returns the chunk index; the bounds
     * of the chosen chunk are updated in place (max_time extension). Pure
     * and unit-testable. */
    static size_t select_chunk(ql::time_chunk_index_t *chunk_index,
                               const ql::time_series_config_t &config,
                               uint64_t chunk_target_rows,
                               uint64_t ts_us);

    /* Total rows across all chunks. */
    static uint64_t total_rows(const ql::time_chunk_index_t &chunk_index);

    /* ── retention + compaction (PHASE3-TS-4, spec §5.2/§6.4/§7) ───────── */

    /* Indices of chunks whose newest row is strictly older than the
     * retention cutoff (chunk.max_time_us < now_us - retention; exactly-
     * at-TTL chunks are kept, spec §8.1). retention_seconds == 0 disables
     * retention. Chunks are time-ordered, so the expired chunks form a
     * prefix of the index and the scan stops at the first live chunk. A
     * clock-underflow guard (now before the retention span has elapsed)
     * clamps the cutoff to 0, expiring nothing. */
    static std::vector<size_t> expired_chunks(
        const ql::time_chunk_index_t &chunk_index,
        uint64_t now_us, uint64_t retention_seconds);

    /* Indices of sealed (non-newest) chunks with rows whose newest row is
     * at least min_age_seconds old — compaction candidates (§6.4: "chunk
     * sealed + 1 hour old"). The active newest chunk is never compacted. */
    static std::vector<size_t> compactible_chunks(
        const ql::time_chunk_index_t &chunk_index,
        uint64_t now_us, uint64_t min_age_seconds);

    /* §7 corruption detection: the index claims rows for a chunk but no
     * tree root exists to hold them. */
    static bool chunk_is_corrupt(const ql::time_chunk_bounds_t &chunk);

    /* Erases one chunk's tree (bounded batches) and removes the chunk from
     * the index. The caller commits the catalog save in the same
     * transaction, so each chunk's erase + index removal are atomic: the
     * index is the checkpoint and a crash rolls the in-flight chunk back,
     * letting the next pass resume from the front. Raises
     * TIME_SERIES_CHUNK_CORRUPT before modifying anything when the chunk
     * is corrupt. Returns the number of rows freed; per-row modification
     * reports are appended for secondary-index maintenance. */
    static uint64_t expire_chunk(
        btree_slice_t *slice, real_superblock_t *sb,
        time_series_catalog_t *catalog, size_t chunk_idx,
        const deletion_context_t *deletion_context, signal_t *interruptor,
        std::vector<rdb_modification_report_t> *mod_reports_out);

    /* Compaction rewrite (§6.4): collects a sealed chunk's rows, rebuilds
     * a fresh packed B-tree, frees the old tree's blocks, and publishes
     * the new root — all in the caller's transaction. Rows and row_count
     * are preserved; the returned count is the number of rows rewritten.
     * Raises TIME_SERIES_CHUNK_CORRUPT on a corrupt chunk. */
    static uint64_t compact_chunk(
        btree_slice_t *slice, real_superblock_t *sb,
        time_series_catalog_t *catalog, size_t chunk_idx,
        const deletion_context_t *deletion_context, signal_t *interruptor);

    /* ── downsampling (PHASE3-TS-5, spec §4.3/§5.3/§6.1/§6.4) ─────────── */

    /* Indices of sealed chunks that still need a downsample merge: every
     * non-newest chunk with rows whose `merged` watermark is false. The
     * active newest chunk is never merged (same rule as compaction). */
    static std::vector<size_t> downsample_candidates(
        const ql::time_chunk_index_t &chunk_index);

    /* Collects every (key, datum) pair of one chunk's tree. Memory is
     * bounded by one chunk's data (§9.3). Raises TIME_SERIES_CHUNK_CORRUPT
     * on a corrupt chunk (validated before any traversal). */
    static std::vector<std::pair<store_key_t, ql::datum_t>> collect_chunk_rows(
        btree_slice_t *slice, real_superblock_t *sb,
        const time_series_catalog_t *catalog, size_t chunk_idx,
        signal_t *interruptor);

    /* Writes downsample rows into a step's tree (rdb_set in ascending key
     * order for append locality, same as the compaction rebuild). `root_in_out`
     * is the tree's root block id; NULL_BLOCK_ID creates the tree. */
    static void write_downsample_rows(
        btree_slice_t *slice, real_superblock_t *sb,
        const std::vector<std::pair<store_key_t, ql::datum_t>> &rows,
        block_id_t *root_in_out, const deletion_context_t *deletion_context);

    /* §6.4: folds one chunk into every downsample step — buckets the
     * chunk's rows per step (bucket = floor(ts / interval) * interval),
     * computes one aggregate row per bucket (the step's aggregate funcs
     * compiled once, per step, not per bucket), writes them into the
     * step's tree via write_downsample_rows, then sets the chunk's
     * `merged` watermark. This is the merge job's per-chunk transaction
     * body: the caller holds the write superblock and saves the catalog in
     * the same transaction, so a crash rolls the in-flight chunk back and
     * the next pass resumes from the watermark. Raises
     * TIME_SERIES_CHUNK_CORRUPT before modifying anything on a corrupt
     * chunk, and lets aggregate-evaluation errors propagate (the chunk
     * stays unmerged). Returns the number of downsample rows written. */
    static uint64_t merge_chunk_into_downsample_steps(
        btree_slice_t *slice, real_superblock_t *sb,
        time_series_catalog_t *catalog, size_t chunk_idx,
        const deletion_context_t *deletion_context, signal_t *interruptor);

    /* Bucket key for a bucket start: the 20-digit zero-padded decimal string
     * of the microseconds value, so lexicographic key order equals
     * chronological order (a fixed width is what makes the key-range bound
     * in `downsample_key_range` exact). */
    static store_key_t downsample_bucket_key(uint64_t bucket_start_us);

    /* Key range covering exactly the downsample buckets overlapping the
     * half-open window [start_us, end_us) for a step of `interval_seconds`.
     * Buckets are keyed by bucket start b = floor(ts / interval) * interval;
     * such a bucket overlaps the window iff b < end_us and b + interval >
     * start_us, and every bucket with
     * floor(start_us / interval) * interval <= b < end_us satisfies both —
     * so the lexicographic range [key(b_first), key(end_us)) is exact.
     * Empty windows (start_us >= end_us) yield an empty range. */
    static key_range_t downsample_key_range(uint64_t start_us, uint64_t end_us,
                                            uint64_t interval_seconds);

    /* §4.3 planner auto-selection: the downsample root to scan for the
     * window [start_us, end_us), or nullptr to fall back to the raw
     * chunk-root path. Applies select_downsample() to the window's range in
     * seconds (strictly-less-than age semantics, verbatim); a selected step
     * is only honored when its tree actually has data (a freshly created
     * root with no rows yet must fall back to raw). */
    static const downsample_root_t *select_downsample_root(
        const time_series_catalog_t &catalog, uint64_t start_us, uint64_t end_us);

    /* §6.1 tableDrop cascade: erases every chunk tree and every downsample
     * tree in bounded batches (O(TIME_SERIES_RETENTION_ERASE_BATCH) per
     * pass, §9.3). The roots are NOT cleared from `catalog` (the caller
     * releases the whole catalog blob afterwards via release_catalog). Row
     * modification reports are appended to `mod_reports_out` so the caller
     * can keep secondary indexes consistent during the erase, mirroring the
     * retention pass. */
    static void release_storage(
        btree_slice_t *slice, real_superblock_t *sb,
        time_series_catalog_t *catalog,
        const deletion_context_t *deletion_context, signal_t *interruptor,
        std::vector<rdb_modification_report_t> *mod_reports_out);
};

RDB_DECLARE_EQUALITY_COMPARABLE(time_series_catalog_t);
RDB_DECLARE_EQUALITY_COMPARABLE(downsample_root_t);

#endif  // BTREE_TIME_SERIES_OPS_HPP_
