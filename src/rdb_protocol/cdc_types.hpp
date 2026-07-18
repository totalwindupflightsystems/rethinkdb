// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_CDC_TYPES_HPP_
#define RDB_PROTOCOL_CDC_TYPES_HPP_

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "containers/uuid.hpp"
#include "rpc/serialize_macros.hpp"
#include "time.hpp"

namespace ql {

class datum_t;

// ── Identity and position types ──

// Shard-local monotonically increasing log sequence number.
// Never reused after crash recovery, compaction, or failover.
struct log_sequence_number_t {
    uint64_t value = 0;

    bool operator<(const log_sequence_number_t &o) const { return value < o.value; }
    bool operator==(const log_sequence_number_t &o) const { return value == o.value; }
    bool operator!=(const log_sequence_number_t &o) const { return value != o.value; }
    bool operator<=(const log_sequence_number_t &o) const { return value <= o.value; }
};

struct shard_lsn_t {
    uuid_u shard_id;
    log_sequence_number_t lsn;
};

// Immutable idempotence key sent with every change record.
struct change_event_id_t {
    uuid_u source_cluster_id;
    uuid_u table_id;
    uuid_u shard_id;
    log_sequence_number_t lsn;

    bool operator<(const change_event_id_t &o) const {
        if (shard_id < o.shard_id) return true;
        if (o.shard_id < shard_id) return false;
        return lsn < o.lsn;
    }
    bool operator==(const change_event_id_t &o) const {
        return source_cluster_id == o.source_cluster_id
            && table_id == o.table_id
            && shard_id == o.shard_id
            && lsn == o.lsn;
    }
    bool operator!=(const change_event_id_t &o) const { return !(*this == o); }
};

// Comparator for std::set ordering by shard and LSN.
struct change_event_id_compare_by_lsn_t {
    bool operator()(const change_event_id_t &a, const change_event_id_t &b) const {
        if (a.shard_id < b.shard_id) return true;
        if (b.shard_id < a.shard_id) return false;
        return a.lsn < b.lsn;
    }
};

// Row mutation operations captured by CDC.
enum class change_operation_t { INSERT = 0, UPDATE = 1, DELETE = 2, REPLACE = 3 };

ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    change_operation_t, int8_t,
    change_operation_t::INSERT, change_operation_t::REPLACE);

// Row mutation before/after image.
struct change_record_t {
    change_event_id_t event_id;
    change_operation_t op;
    std::vector<char> before_image;  // empty for INSERT
    std::vector<char> after_image;   // empty for DELETE
    microtime_t commit_timestamp;
};

// ── Datum image serialization helpers ──
//
// A change_record_t's before_image / after_image stores the serialized form
// of a datum_t in a self-contained std::vector<char>. We piggyback on the
// stable datum_serialize / datum_deserialize routines so the bytes are
// interpretable across processes and across releases of the wire format.
//
// CDC-04 will replace these with a journal-backed representation; for now
// these helpers are sufficient to stage the capture seam in the write path.
//
// Declarations only — implementations live in src/rdb_protocol/store.cc
// where the full datum_t definition is available.

// Serialize a datum_t into a self-contained vector<char>.
// Returns an empty vector when given a datum with no value (e.g. an
// uninitialized datum_t on INSERT).
std::vector<char> serialize_datum_to_vector(const datum_t &d);

// Deserialize a vector<char> back into a datum_t. An empty input yields
// an uninitialized datum_t.
datum_t deserialize_datum_from_vector(const std::vector<char> &v);

}  // namespace ql

RDB_DECLARE_SERIALIZABLE(ql::log_sequence_number_t);
RDB_DECLARE_SERIALIZABLE(ql::shard_lsn_t);
RDB_DECLARE_SERIALIZABLE(ql::change_event_id_t);
RDB_DECLARE_SERIALIZABLE(ql::change_record_t);

#endif  // RDB_PROTOCOL_CDC_TYPES_HPP_
