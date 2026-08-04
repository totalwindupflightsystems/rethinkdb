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
struct point_write_response_t;
struct rdb_modification_info_t;

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

/* Durable time-series catalog blob. Serialized at LATEST_DISK like the
 * partition catalog (Phase-3 data never existed in an older format). */
struct time_series_catalog_t {
    uint32_t format_version;
    ql::time_series_config_t config;
    ql::time_chunk_index_t chunk_index;

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
};

RDB_DECLARE_EQUALITY_COMPARABLE(time_series_catalog_t);

#endif  // BTREE_TIME_SERIES_OPS_HPP_
