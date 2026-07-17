// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_PARTITION_CONFIG_HPP_
#define RDB_PROTOCOL_PARTITION_CONFIG_HPP_

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "btree/keys.hpp"
#include "containers/archive/archive.hpp"
#include "containers/name_string.hpp"
#include "containers/uuid.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/datumspec.hpp"
#include "rdb_protocol/serialize_datum.hpp"
#include "rpc/semilattice/joins/macros.hpp"
#include "rpc/serialize_macros.hpp"

/* Hard limit on active partitions per logical table (Phase 3 design). */
static constexpr size_t PARTITION_MAX_COUNT = 128;

/* Hash modulus bounds for hash partitioning. */
static constexpr uint32_t PARTITION_HASH_MODULUS_MIN = 2;
static constexpr uint32_t PARTITION_HASH_MODULUS_MAX = 65536;

enum class partition_type_t : int8_t {
    NONE = 0,
    RANGE = 1,
    HASH = 2,
    LIST = 3
};

enum class partition_state_t : int8_t {
    CREATING = 0,
    CATCHING_UP = 1,
    ACTIVE = 2,
    DRAINING = 3,
    FAILED = 4
};

ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    partition_type_t, int8_t,
    partition_type_t::NONE, partition_type_t::LIST);
ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    partition_state_t, int8_t,
    partition_state_t::CREATING, partition_state_t::FAILED);

class partition_entry_t {
public:
    uuid_u id;
    name_string_t name;
    namespace_id_t storage_id;
    partition_state_t state;
    key_range_t primary_key_range;
    std::vector<uint32_t> hash_buckets;
    std::vector<ql::datum_t> list_values;
    bool list_default;

    partition_entry_t()
        : state(partition_state_t::CREATING),
          list_default(false) { }
};

RDB_DECLARE_SERIALIZABLE(partition_entry_t);
RDB_DECLARE_EQUALITY_COMPARABLE(partition_entry_t);

/* Predicate extracted by the query planner for partition pruning.
 * UNKNOWN = cannot prove a restriction (safe fallback: all active).
 * EQUALITY / RANGE only apply when `field` matches config.key_field. */
class partition_predicate_t {
public:
    enum kind_t { UNKNOWN, EQUALITY, RANGE };
    kind_t kind;
    std::string field;           /* partition key field name */
    ql::datum_t equality_value;  /* EQUALITY */
    ql::datum_t range_lo;        /* RANGE: lower bound (inclusive) */
    ql::datum_t range_hi;        /* RANGE: upper bound (exclusive) */

    partition_predicate_t() : kind(UNKNOWN) { }

    static partition_predicate_t make_equality(
            const std::string &f, ql::datum_t val) {
        partition_predicate_t p;
        p.kind = EQUALITY;
        p.field = f;
        p.equality_value = std::move(val);
        return p;
    }

    static partition_predicate_t make_range(
            const std::string &f, ql::datum_t lo, ql::datum_t hi) {
        partition_predicate_t p;
        p.kind = RANGE;
        p.field = f;
        p.range_lo = std::move(lo);
        p.range_hi = std::move(hi);
        return p;
    }
};

class partition_route_t {
public:
    uuid_u partition_id;
    namespace_id_t storage_id;
    uint64_t epoch;

    partition_route_t() : epoch(0) { }
    partition_route_t(const uuid_u &pid, const namespace_id_t &sid, uint64_t e)
        : partition_id(pid), storage_id(sid), epoch(e) { }
};

class partition_selection_t {
public:
    /* Set of partition UUIDs selected by prune() / the planner for fan-out. */
    std::vector<uuid_u> partition_ids;
};

class partition_config_t {
public:
    partition_type_t type;
    std::string key_field;
    uint64_t epoch;
    std::vector<partition_entry_t> partitions;
    std::vector<ql::datum_t> range_boundaries;
    uint32_t hash_modulus;

    partition_config_t()
        : type(partition_type_t::NONE),
          epoch(0),
          hash_modulus(0) { }

    bool is_partitioned() const;
    void validate_or_throw() const;
    const partition_entry_t *route(const ql::datum_t &key) const;
    std::vector<const partition_entry_t *> prune(const partition_predicate_t &) const;
};

RDB_DECLARE_SERIALIZABLE(partition_config_t);
RDB_DECLARE_EQUALITY_COMPARABLE(partition_config_t);

class partition_map_t {
public:
    uint64_t epoch;
    /* partition UUID → primary key range for that partition */
    std::map<uuid_u, key_range_t> primary_ranges;
    /* partition UUID → storage namespace ID */
    std::map<uuid_u, namespace_id_t> stores;

    partition_map_t() : epoch(0) { }

    /* partition UUID → selection of routes for query fan-out.
     * Returns routes for the requested partition_ids present in `stores`. */
    std::vector<partition_route_t> routes_for(const partition_selection_t &) const;
};

/* Stable hash of a partition-key datum into [0, modulus).
 * Uses the durable datum wire format + hash_region_hasher so results are
 * independent of process address space and reql version drift. */
uint32_t partition_hash_bucket(const ql::datum_t &key, uint32_t modulus);

/* True if `d` is a legal partition-key / list value (non-null scalar, or
 * TIME pseudo-type). Objects/arrays/geometry/null/minval/maxval/vector are
 * rejected. */
bool is_legal_partition_key_datum(const ql::datum_t &d);

/* Convert a stored partition_config_t into the public partitionInfo() ReQL
 * object shape. Unpartitioned (type=NONE) returns {partitioned: false}. */
ql::datum_t partition_config_to_datum(const partition_config_t &config);

#endif  // RDB_PROTOCOL_PARTITION_CONFIG_HPP_
