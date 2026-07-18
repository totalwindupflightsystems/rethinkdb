// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef SERIALIZER_LOGICAL_JOURNAL_HPP_
#define SERIALIZER_LOGICAL_JOURNAL_HPP_

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "arch/compiler.hpp"
#include "concurrency/signal.hpp"
#include "containers/uuid.hpp"
#include "perfmon/core.hpp"
#include "rdb_protocol/cdc_types.hpp"
#include "threading.hpp"

class file_account_t;
class io_backender_t;
class serializer_filepath_t;

/*
 * logical_journal_t — CDC-04
 *
 * An append-only, versioned record stream per table shard. Provides
 * monotonic, durable LSN allocation; atomic append with CRC32C
 * integrity checks; checkpoint + recovery against a persistent
 * LSN->file-offset index; and per-shard snapshot barriers (via the
 * `allocate_lsn()` helper).
 *
 * CDC-04 uses ordinary POSIX files (O_APPEND + fsync + atomic
 * rename) rather than the serializer extent manager. The integration
 * with the metablock format is left for CDC-08 (Phase 5 / retention),
 * which needs to store LSN watermarks in the metablock anyway.
 *
 * Threading: a logical_journal_t is bound to a single home thread
 * (the thread that owns the underlying serializer store). All
 * mutating operations must be called from that thread. Recovery and
 * read paths use the home thread assertion in debug builds but are
 * otherwise synchronous.
 */

class logical_journal_t : public home_thread_mixin_t {
public:
    logical_journal_t(const serializer_filepath_t &serializer_dir,
                      const uuid_u &table_id,
                      const uuid_u &shard_id,
                      io_backender_t *io_backender,
                      perfmon_collection_t *perfmon_collection);
    ~logical_journal_t();

    // Create journal directory + files. Idempotent — safe to call on
    // an already-initialized journal (recovery is then a separate
    // step).
    void create(signal_t *interruptor);

    // Recover from checkpoint. Validates tail, discards incomplete
    // records, sets high_water_lsn to the last LSN of the last index
    // entry. Must be called after create() and before any
    // append_records().
    void recover(signal_t *interruptor);

    // Append a batch of change records atomically. Returns the first
    // LSN allocated for this batch. The records receive consecutive
    // LSNs `first`, `first+1`, ..., `first+records.size()-1`.
    //
    // Caller must call `checkpoint()` after the enclosing transaction
    // commits, and `high_water_rollback()` if it rolls back.
    ql::log_sequence_number_t append_records(
        const std::vector<ql::change_record_t> &records,
        file_account_t *io_account,
        signal_t *interruptor);

    // Allocate next LSN without writing any record bytes. Useful for
    // snapshot barriers (CDC-06) and for empty mutation streams.
    ql::log_sequence_number_t allocate_lsn();

    // Current durable high-water LSN (i.e. last LSN for which bytes
    // are on disk). Returns 0 before create/recover.
    ql::log_sequence_number_t high_water_lsn() const;

    // Roll back to the last checkpointed position. Truncates the
    // data file and pops the in-memory index back to the last
    // checkpointed entry. Used when the enclosing transaction aborts.
    void high_water_rollback(signal_t *interruptor);

    // Read all committed records since `since_lsn` (exclusive), in
    // LSN order. If `since_lsn` is at or beyond the high-water mark,
    // returns an empty vector.
    std::vector<ql::change_record_t> read_from(
        ql::log_sequence_number_t since_lsn,
        signal_t *interruptor) const;

    // Persist the in-memory index to disk. MUST be called after every
    // successful commit of an enclosing transaction that appended
    // records, otherwise those records are recoverable only via
    // tail-scanning of the data file.
    void checkpoint(file_account_t *io_account, signal_t *interruptor);

    // Observability: total bytes used in journal.data (including
    // record framing + CRC32C overhead).
    uint64_t retained_bytes() const;

    // Observability: number of records currently in the in-memory
    // index (i.e. records that survive a crash if the last checkpoint
    // has been persisted).
    uint64_t record_count() const;

private:
    struct index_entry_t {
        ql::log_sequence_number_t first_lsn;
        ql::log_sequence_number_t last_lsn;
        uint64_t file_offset;
        uint32_t byte_length;
    };

    ATTR_PACKED(struct record_header_t {
        uint8_t  format_version;
        uint32_t total_length;
        uint32_t crc32;
    });

    enum { RECORD_FORMAT_VERSION = 1 };

    void serialize_record(const ql::change_record_t &rec,
                          std::vector<char> *out) const;
    bool deserialize_record(const std::vector<char> &data,
                            size_t *offset,
                            ql::change_record_t *out) const;
    static uint32_t compute_crc32(const char *data, size_t len);
    std::vector<char> read_entire_file(const std::string &path,
                                       signal_t *interruptor) const;
    void truncate_data_file(uint64_t byte_offset, signal_t *interruptor);

    // ── State ──
    uuid_u table_id_;
    uuid_u shard_id_;
    std::string journal_dir_;
    std::string data_path_;
    std::string idx_path_;
    std::string idx_tmp_path_;
    io_backender_t *const io_backender_;
    perfmon_collection_t *const perfmon_collection_;

    std::atomic<uint64_t> high_water_lsn_val_{0};
    std::vector<index_entry_t> index_;
    // Number of index entries covered by the last successful
    // checkpoint. Used by high_water_rollback() to know how far back
    // to truncate when an in-progress transaction aborts.
    size_t checkpointed_size_{0};
    uint64_t data_file_size_{0};
    bool created_{false};
    bool recovered_{false};

    DISABLE_COPYING(logical_journal_t);
};

#endif  // SERIALIZER_LOGICAL_JOURNAL_HPP_