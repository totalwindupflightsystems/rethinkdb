// Copyright 2010-2015 RethinkDB, all rights reserved.
#ifndef BTREE_REQL_SPECIFIC_HPP_
#define BTREE_REQL_SPECIFIC_HPP_

#include "btree/operations.hpp"

/* Most of the code in the `btree/` directory doesn't "know" about the format of the
superblock; instead it manipulates the superblock using the abstract `superblock_t`. This
file provides the concrete superblock implementation used for ReQL primary and sindex
B-trees. It also provides functions for working with the secondary index block and the
metainfo, which are unrelated to the B-tree but stored on the ReQL primary superblock.

`btree/secondary_operations.*` and `btree/reql_specific.*` are the only files in the
`btree/` directory that know about ReQL-specific concepts such as metainfo and sindexes.
They should probably be moved out of the `btree/` directory. */

class binary_blob_t;

/* -- Partition catalog types (Phase 3 declarative partitioning) -- */

#include "containers/uuid.hpp"
#include "rdb_protocol/partition_config.hpp"
#include "rdb_protocol/protocol.hpp"
#include "rpc/serialize_macros.hpp"

/* On-disk catalog format version for partition_catalog_t. Bump when the
serialized layout of the catalog or its store refs changes incompatibly. */
static constexpr uint32_t PARTITION_CATALOG_FORMAT_VERSION = 1;

struct partition_store_ref_t {
    uuid_u partition_id;
    namespace_id_t storage_id;
    std::vector<block_id_t> shard_superblocks;
    /* Lifecycle state for this store (CREATING / CATCHING_UP / ACTIVE /
    DRAINING / FAILED). Tracked on the store ref so the catalog can hold both
    live and draining stores across an epoch cutover. */
    partition_state_t state;
    /* Epoch that published this store. DRAINING stores from epoch E may be
    retired once minimum_live_epoch > E. */
    uint64_t epoch;

    partition_store_ref_t()
        : state(partition_state_t::CREATING),
          epoch(0) { }
};
RDB_DECLARE_SERIALIZABLE(partition_store_ref_t);

/* One durable entry in the transition modification queue (PART-07). Written
for every source-epoch mutation while a repartition is in progress so target
replay can catch up without blocking ordinary writes. */
struct transition_modification_t {
    store_key_t primary_key;
    /* New document value, or a null/empty datum for a delete. */
    ql::datum_t value;
    /* Monotonic stamp assigned at enqueue; target replay is idempotent by
    (primary_key, mutation_stamp). */
    uint64_t mutation_stamp;

    transition_modification_t()
        : mutation_stamp(0) { }
};
RDB_DECLARE_SERIALIZABLE(transition_modification_t);

struct partition_catalog_t {
    uint32_t format_version;
    uint64_t epoch;
    block_id_t primary_key_directory_block;
    std::vector<partition_store_ref_t> stores;

    /* PART-07: durable transition modification queue, stored alongside the
    catalog blob so snapshot+replay survives crashes. Empty / inactive when no
    repartition is running. Cleared after successful cutover or fail cleanup. */
    bool transition_active;
    uint64_t transition_source_epoch;
    /* Next stamp to assign on enqueue; starts at 1 when a transition begins. */
    uint64_t next_mutation_stamp;
    /* High-water mark recorded after snapshot copy; replay applies entries
    with stamp <= high_water_mark. Zero until snapshot completes. */
    uint64_t high_water_mark;
    std::vector<transition_modification_t> transition_queue;

    partition_catalog_t()
        : format_version(0),
          epoch(0),
          primary_key_directory_block(NULL_BLOCK_ID),
          transition_active(false),
          transition_source_epoch(0),
          next_mutation_stamp(1),
          high_water_mark(0) { }
};
RDB_DECLARE_SERIALIZABLE(partition_catalog_t);

/* `real_superblock_t` represents the superblock for the primary B-tree of a table. */
class real_superblock_t : public superblock_t {
public:
    explicit real_superblock_t(buf_lock_t &&sb_buf);
    real_superblock_t(buf_lock_t &&sb_buf, new_semaphore_in_line_t &&write_semaphore_acq);

    void release();
    buf_lock_t *get() { return &sb_buf_; }

    block_id_t get_root_block_id();
    void set_root_block_id(block_id_t new_root_block);

    block_id_t get_stat_block_id();

    block_id_t get_sindex_block_id();

    /* Table-level partition catalog blob (Phase 3 declarative partitioning).
    NULL_BLOCK_ID means the table is not partitioned / catalog not published. */
    block_id_t get_partition_catalog_block_id();
    void set_partition_catalog_block_id(block_id_t new_id);

    buf_parent_t expose_buf() { return buf_parent_t(&sb_buf_); }

private:
    /* The write_semaphore_acq_ is empty for reads.
    For writes it locks the write superblock acquisition semaphore until the
    sb_buf_ is released.
    Note that this is used to throttle writes compared to reads, but not required
    for correctness. */
    new_semaphore_in_line_t write_semaphore_acq_;

    buf_lock_t sb_buf_;
};

/* `sindex_superblock_t` represents the superblock for a sindex B-tree. */
class sindex_superblock_t : public superblock_t {
public:
    explicit sindex_superblock_t(buf_lock_t &&sb_buf);

    void release();
    buf_lock_t *get() { return &sb_buf_; }

    block_id_t get_root_block_id();
    void set_root_block_id(block_id_t new_root_block);

    block_id_t get_stat_block_id();

    /* sindex superblocks shouldn't have a sindex block of their own. But in previous
    versions of RethinkDB they would have an empty sindex block. This is exposed so that
    we can delete such a sindex block when deleting the sindex superblock. */
    block_id_t get_sindex_block_id();

    /* For vector-type secondary indexes, this points to a child block that holds the
    serialized HNSW graph (as a blob). NULL_BLOCK_ID means no graph (e.g. for non-vector
    indexes or for indexes where graph construction failed/was skipped). */
    block_id_t get_vector_graph_block_id();
    void set_vector_graph_block_id(block_id_t new_id);

    /* For BRIN-type secondary indexes, this points to a child block that holds the
    serialized brin_index_t sidecar blob. NULL_BLOCK_ID means no sidecar. */
    block_id_t get_brin_summary_block_id();
    void set_brin_summary_block_id(block_id_t new_id);

    buf_parent_t expose_buf() { return buf_parent_t(&sb_buf_); }

private:
    buf_lock_t sb_buf_;
};

enum class index_type_t {
    PRIMARY,
    SECONDARY
};

/* btree_slice_t is a thin wrapper around cache_t that handles initializing the buffer
cache for the purpose of storing a B-tree. It is specific to ReQL primary and secondary
index B-trees. */

class btree_slice_t : public home_thread_mixin_debug_only_t {
public:
    // Initializes a superblock (presumably, a buf_lock_t constructed with
    // alt_create_t::create) for use with btrees, setting the initial value of the
    // metainfo (with a single key/value pair). Not for use with sindex superblocks.
    static void init_real_superblock(real_superblock_t *superblock,
                                const std::vector<char> &metainfo_key,
                                const binary_blob_t &metainfo_value);
    static void init_sindex_superblock(sindex_superblock_t *superblock);

    btree_slice_t(cache_t *cache,
                  perfmon_collection_t *parent,
                  const std::string &identifier,
                  index_type_t index_type);

    ~btree_slice_t();

    cache_t *cache() { return cache_; }
    cache_account_t *get_backfill_account() { return &backfill_account_; }

    btree_stats_t stats;

private:
    cache_t *cache_;

    // Cache account to be used when backfilling.
    cache_account_t backfill_account_;

    DISABLE_COPYING(btree_slice_t);
};

/* This iterator encapsulates most of the metainfo data layout. Unfortunately,
 * functions set_superblock_metainfo and delete_superblock_metainfo also know a
 * lot about the data layout, so if it's changed, these functions must be
 * changed as well.
 *
 * Data layout is dead simple right now, it's an array of the following
 * (unaligned, unpadded) contents:
 *
 *   sz_t key_size;
 *   char key[key_size];
 *   sz_t value_size;
 *   char value[value_size];
 */
struct superblock_metainfo_iterator_t {
    typedef uint32_t sz_t;  // be careful: the values of this type get casted to int64_t in checks, so it must fit
    typedef std::pair<sz_t, char *> key_t;
    typedef std::pair<sz_t, char *> value_t;

    superblock_metainfo_iterator_t(char *metainfo, char *metainfo_end) : end(metainfo_end) { advance(metainfo); }

    bool is_end() { return pos == end; }

    void operator++();

    std::pair<key_t, value_t> operator*() {
        return std::make_pair(key(), value());
    }
    key_t key() { return std::make_pair(key_size, key_ptr); }
    value_t value() { return std::make_pair(value_size, value_ptr); }

    char *record_ptr() { return pos; }
    char *next_record_ptr() { return next_pos; }
    char *end_ptr() { return end; }
    sz_t *value_size_ptr() { return reinterpret_cast<sz_t*>(value_ptr) - 1; }
private:
    void advance(char *p);

    char *pos;
    char *next_pos;
    char *end;
    sz_t key_size;
    char *key_ptr;
    sz_t value_size;
    char *value_ptr;
};


// Metainfo functions
void get_superblock_metainfo(
    real_superblock_t *superblock,
    std::vector< std::pair<std::vector<char>, std::vector<char> > > *kv_pairs_out,
    cluster_version_t *version_out);

void set_superblock_metainfo(real_superblock_t *superblock,
                             const std::vector<char> &key,
                             const binary_blob_t &value,
                             cluster_version_t version);

void set_superblock_metainfo(real_superblock_t *superblock,
                             const std::vector<std::vector<char> > &keys,
                             const std::vector<binary_blob_t> &values,
                             cluster_version_t version);

// Convenience functions for accessing the superblock
void get_btree_superblock(
        txn_t *txn,
        access_t access,
        scoped_ptr_t<real_superblock_t> *got_superblock_out);

/* Variant for writes that go through a superblock write semaphore */
void get_btree_superblock(
        txn_t *txn,
        write_access_t access,
        new_semaphore_in_line_t &&write_sem_acq,
        scoped_ptr_t<real_superblock_t> *got_superblock_out);

void get_btree_superblock_and_txn_for_writing(
        cache_conn_t *cache_conn,
        new_semaphore_t *superblock_write_semaphore,
        write_access_t superblock_access,
        int expected_change_count,
        write_durability_t durability,
        scoped_ptr_t<real_superblock_t> *got_superblock_out,
        scoped_ptr_t<txn_t> *txn_out);

void get_btree_superblock_and_txn_for_backfilling(
        cache_conn_t *cache_conn,
        cache_account_t *backfill_account,
        scoped_ptr_t<real_superblock_t> *got_superblock_out,
        scoped_ptr_t<txn_t> *txn_out);

void get_btree_superblock_and_txn_for_reading(
        cache_conn_t *cache_conn,
        cache_snapshotted_t snapshotted,
        scoped_ptr_t<real_superblock_t> *got_superblock_out,
        scoped_ptr_t<txn_t> *txn_out);

#endif // BTREE_REQL_SPECIFIC_HPP_

