// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/partition_config.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include "containers/archive/stl_types.hpp"
#include "containers/archive/vector_stream.hpp"
#include "containers/archive/versioned.hpp"
#include "rdb_protocol/error.hpp"
#include "rdb_protocol/partition_errors.hpp"
#include "rdb_protocol/pseudo_time.hpp"

/* ── helpers ─────────────────────────────────────────────────────────────── */

/* Local copy of hash_region_hasher's byte-stream algorithm. The 2-arg form is
 * not exported from hash_region.hpp; reimplementing keeps the partition hash
 * stable and dependency-free. */
uint64_t partition_byte_hash(const uint8_t *s, ssize_t len) {
    rassert(len >= 0);
    uint64_t h = 0x47a59e381fb2dc06ULL;
    for (ssize_t i = 0; i < len; ++i) {
        const uint8_t ch = s[i];
        const uint64_t d =
            (((ch * 0x80200802ULL) & 0x0884422110ULL) * 0x0101010101ULL) << 23;
        h += d;
        h = h ^ (h >> 11) ^ (h << 21);
    }
    return h & 0x7fffffffffffffffULL;
}

bool is_legal_partition_key_datum(const ql::datum_t &d) {
    if (!d.has()) {
        return false;
    }
    switch (d.get_type()) {
    case ql::datum_t::R_BOOL:
    case ql::datum_t::R_NUM:
    case ql::datum_t::R_STR:
    case ql::datum_t::R_BINARY:
        return true;
    case ql::datum_t::R_OBJECT:
        /* TIME is allowed (range/list keys often use timestamps). Geometry
         * and other pseudo-types are not legal partition keys. */
        return d.is_ptype(ql::pseudo::time_string);
    case ql::datum_t::UNINITIALIZED:
    case ql::datum_t::MINVAL:
    case ql::datum_t::MAXVAL:
    case ql::datum_t::R_ARRAY:
    case ql::datum_t::R_NULL:
    case ql::datum_t::R_VECTOR:
        return false;
    default:
        return false;
    }
}

uint32_t partition_hash_bucket(const ql::datum_t &key, uint32_t modulus) {
    guarantee(modulus >= PARTITION_HASH_MODULUS_MIN);
    write_message_t wm;
    ql::datum_serialize(&wm, key, ql::check_datum_serialization_errors_t::NO);
    vector_stream_t stream;
    int res = send_write_message(&stream, &wm);
    guarantee(res == 0);
    const std::vector<char> &bytes = stream.vector();
    const uint64_t h = partition_byte_hash(
        reinterpret_cast<const uint8_t *>(bytes.data()),
        static_cast<ssize_t>(bytes.size()));
    return static_cast<uint32_t>(h % modulus);
}

/* ── equality ────────────────────────────────────────────────────────────── */

RDB_IMPL_EQUALITY_COMPARABLE_8(
    partition_entry_t,
    id, name, storage_id, state, primary_key_range,
    hash_buckets, list_values, list_default);

RDB_IMPL_EQUALITY_COMPARABLE_6(
    partition_config_t,
    type, key_field, epoch, partitions, range_boundaries, hash_modulus);

/* ── serialization ───────────────────────────────────────────────────────── */

template <cluster_version_t W>
void serialize(write_message_t *wm, const partition_entry_t &thing) {
    serialize<W>(wm, thing.id);
    serialize<W>(wm, thing.name);
    serialize<W>(wm, thing.storage_id);
    serialize<W>(wm, thing.state);
    serialize<W>(wm, thing.primary_key_range);
    serialize<W>(wm, thing.hash_buckets);
    serialize<W>(wm, thing.list_values);
    serialize<W>(wm, thing.list_default);
}

template <cluster_version_t W>
archive_result_t deserialize(read_stream_t *s, partition_entry_t *thing) {
    archive_result_t res = deserialize<W>(s, &thing->id);
    if (bad(res)) { return res; }
    res = deserialize<W>(s, &thing->name);
    if (bad(res)) { return res; }
    res = deserialize<W>(s, &thing->storage_id);
    if (bad(res)) { return res; }
    res = deserialize<W>(s, &thing->state);
    if (bad(res)) { return res; }
    res = deserialize<W>(s, &thing->primary_key_range);
    if (bad(res)) { return res; }
    res = deserialize<W>(s, &thing->hash_buckets);
    if (bad(res)) { return res; }
    res = deserialize<W>(s, &thing->list_values);
    if (bad(res)) { return res; }
    res = deserialize<W>(s, &thing->list_default);
    if (bad(res)) { return res; }
    return res;
}

INSTANTIATE_SERIALIZABLE_SINCE_v2_4(partition_entry_t);

template <cluster_version_t W>
void serialize(write_message_t *wm, const partition_config_t &thing) {
    serialize<W>(wm, thing.type);
    serialize<W>(wm, thing.key_field);
    serialize<W>(wm, thing.epoch);
    serialize<W>(wm, thing.partitions);
    serialize<W>(wm, thing.range_boundaries);
    serialize<W>(wm, thing.hash_modulus);
}

template <cluster_version_t W>
archive_result_t deserialize(read_stream_t *s, partition_config_t *thing) {
    archive_result_t res = deserialize<W>(s, &thing->type);
    if (bad(res)) { return res; }
    res = deserialize<W>(s, &thing->key_field);
    if (bad(res)) { return res; }
    res = deserialize<W>(s, &thing->epoch);
    if (bad(res)) { return res; }
    res = deserialize<W>(s, &thing->partitions);
    if (bad(res)) { return res; }
    res = deserialize<W>(s, &thing->range_boundaries);
    if (bad(res)) { return res; }
    res = deserialize<W>(s, &thing->hash_modulus);
    if (bad(res)) { return res; }
    return res;
}

INSTANTIATE_SERIALIZABLE_SINCE_v2_4(partition_config_t);

/* ── validation ──────────────────────────────────────────────────────────── */

bool partition_config_t::is_partitioned() const {
    return type != partition_type_t::NONE;
}

using partition_error_code::config_invalid;
using partition_error_code::hash_invalid;
using partition_error_code::list_invalid;
using partition_error_code::range_invalid;
using partition_error_code::raise_logic;

void partition_config_t::validate_or_throw() const {
    if (type == partition_type_t::NONE) {
        if (!partitions.empty()) {
            raise_logic(config_invalid,
                "Unpartitioned table config (type=NONE) must have an empty "
                "partitions vector.");
        }
        if (!range_boundaries.empty()) {
            raise_logic(config_invalid,
                "Unpartitioned table config (type=NONE) must have empty "
                "range_boundaries.");
        }
        if (hash_modulus != 0) {
            raise_logic(config_invalid,
                "Unpartitioned table config (type=NONE) must have "
                "hash_modulus=0.");
        }
        return;
    }

    if (key_field.empty()) {
        raise_logic(config_invalid,
            "Partition key field (`by`) must be a non-empty string.");
    }

    if (partitions.empty()) {
        raise_logic(config_invalid,
            "Partitioned table config must define at least one partition.");
    }

    if (partitions.size() > PARTITION_MAX_COUNT) {
        raise_logic(config_invalid,
            "A table may have at most %zu partitions (got %zu).",
            PARTITION_MAX_COUNT, partitions.size());
    }

    /* Unique names. */
    {
        std::set<std::string> seen_names;
        for (const partition_entry_t &p : partitions) {
            if (p.name.empty()) {
                raise_logic(config_invalid,
                    "Partition names must be non-empty.");
            }
            if (!seen_names.insert(p.name.str()).second) {
                raise_logic(config_invalid,
                    "Duplicate partition name `%s`.", p.name.c_str());
            }
        }
    }

    switch (type) {
    case partition_type_t::NONE:
        unreachable();

    case partition_type_t::RANGE: {
        /* N+1 boundaries define N partitions. */
        if (range_boundaries.size() != partitions.size() + 1) {
            raise_logic(range_invalid,
                "Range partitioning requires N+1 boundaries for N partitions "
                "(got %zu boundaries for %zu partitions).",
                range_boundaries.size(), partitions.size());
        }
        if (range_boundaries.front() != ql::datum_t::minval()) {
            raise_logic(range_invalid,
                "Range partition boundaries must start at r.minval.");
        }
        if (range_boundaries.back() != ql::datum_t::maxval()) {
            raise_logic(range_invalid,
                "Range partition boundaries must end at r.maxval.");
        }
        for (size_t i = 0; i + 1 < range_boundaries.size(); ++i) {
            if (!range_boundaries[i].has() || !range_boundaries[i + 1].has()) {
                raise_logic(range_invalid,
                    "Range partition boundaries must be initialized datums.");
            }
            if (!(range_boundaries[i] < range_boundaries[i + 1])) {
                raise_logic(range_invalid,
                    "Range partition boundaries must be strictly sorted and "
                    "define nonempty [from, to) intervals.");
            }
        }
        if (hash_modulus != 0) {
            raise_logic(range_invalid,
                "Range partitioning must have hash_modulus=0.");
        }
        for (const partition_entry_t &p : partitions) {
            if (!p.hash_buckets.empty()) {
                raise_logic(range_invalid,
                    "Range partitions must not carry hash_buckets.");
            }
            if (!p.list_values.empty() || p.list_default) {
                raise_logic(range_invalid,
                    "Range partitions must not carry list values/default.");
            }
        }
        break;
    }

    case partition_type_t::HASH: {
        if (hash_modulus < PARTITION_HASH_MODULUS_MIN
            || hash_modulus > PARTITION_HASH_MODULUS_MAX) {
            raise_logic(hash_invalid,
                "Hash partition modulus must be in [%u, %u] (got %u).",
                PARTITION_HASH_MODULUS_MIN, PARTITION_HASH_MODULUS_MAX,
                hash_modulus);
        }
        if (!range_boundaries.empty()) {
            raise_logic(hash_invalid,
                "Hash partitioning must have empty range_boundaries.");
        }
        std::vector<bool> seen(hash_modulus, false);
        size_t covered = 0;
        for (const partition_entry_t &p : partitions) {
            if (!p.list_values.empty() || p.list_default) {
                raise_logic(hash_invalid,
                    "Hash partitions must not carry list values/default.");
            }
            for (uint32_t bucket : p.hash_buckets) {
                if (bucket >= hash_modulus) {
                    raise_logic(hash_invalid,
                        "Hash bucket %u is out of range for modulus %u.",
                        bucket, hash_modulus);
                }
                if (seen[bucket]) {
                    raise_logic(hash_invalid,
                        "Hash bucket %u is assigned to more than one partition.",
                        bucket);
                }
                seen[bucket] = true;
                ++covered;
            }
        }
        if (covered != static_cast<size_t>(hash_modulus)) {
            raise_logic(hash_invalid,
                "Hash partitioning must assign every bucket in [0, %u) "
                "exactly once (covered %zu of %u).",
                hash_modulus, covered, hash_modulus);
        }
        break;
    }

    case partition_type_t::LIST: {
        if (hash_modulus != 0) {
            raise_logic(list_invalid,
                "List partitioning must have hash_modulus=0.");
        }
        if (!range_boundaries.empty()) {
            raise_logic(list_invalid,
                "List partitioning must have empty range_boundaries.");
        }
        size_t default_count = 0;
        /* Compare list values by datum equality; track via linear scan since
         * datum_t is not hashable out of the box and P is <= 128. */
        std::vector<ql::datum_t> all_values;
        for (const partition_entry_t &p : partitions) {
            if (!p.hash_buckets.empty()) {
                raise_logic(list_invalid,
                    "List partitions must not carry hash_buckets.");
            }
            if (p.list_default) {
                ++default_count;
            }
            for (const ql::datum_t &v : p.list_values) {
                if (!is_legal_partition_key_datum(v)) {
                    raise_logic(list_invalid,
                        "List partition values must be non-null scalars "
                        "(or TIME); objects, arrays, geometry, null, minval, "
                        "and maxval are not allowed.");
                }
                for (const ql::datum_t &prev : all_values) {
                    if (prev == v) {
                        raise_logic(list_invalid,
                            "List partition value %s appears in more than one "
                            "partition.",
                            v.trunc_print().c_str());
                    }
                }
                all_values.push_back(v);
            }
        }
        if (default_count != 1) {
            raise_logic(list_invalid,
                "List partitioning requires exactly one default partition "
                "(got %zu).",
                default_count);
        }
        break;
    }

    default:
        raise_logic(config_invalid, "Unknown partition type.");
    }
}

/* ── routing ─────────────────────────────────────────────────────────────── */

const partition_entry_t *partition_config_t::route(
        const ql::datum_t &key) const {
    switch (type) {
    case partition_type_t::NONE:
        return nullptr;

    case partition_type_t::RANGE: {
        /* N+1 boundaries define N partitions: entry i owns
         * [boundaries[i], boundaries[i+1]). */
        if (range_boundaries.size() < 2
            || partitions.size() + 1 != range_boundaries.size()) {
            return nullptr;
        }
        /* upper_bound returns the first boundary strictly greater than key;
         * the containing interval starts one before that. */
        auto it = std::upper_bound(
            range_boundaries.begin(), range_boundaries.end(), key);
        if (it == range_boundaries.begin()) {
            return nullptr;
        }
        const size_t idx = static_cast<size_t>(
            std::distance(range_boundaries.begin(), it)) - 1;
        /* key == maxval (last boundary) yields idx == partitions.size(). */
        if (idx >= partitions.size()) {
            return nullptr;
        }
        /* Half-open: left closed, right open. */
        if (key < range_boundaries[idx] || !(key < range_boundaries[idx + 1])) {
            return nullptr;
        }
        return &partitions[idx];
    }

    case partition_type_t::HASH: {
        if (hash_modulus < PARTITION_HASH_MODULUS_MIN) {
            return nullptr;
        }
        const uint32_t bucket = partition_hash_bucket(key, hash_modulus);
        for (const partition_entry_t &p : partitions) {
            for (uint32_t b : p.hash_buckets) {
                if (b == bucket) {
                    return &p;
                }
            }
        }
        return nullptr;
    }

    case partition_type_t::LIST: {
        const partition_entry_t *default_part = nullptr;
        for (const partition_entry_t &p : partitions) {
            if (p.list_default) {
                default_part = &p;
            }
            for (const ql::datum_t &v : p.list_values) {
                if (v == key) {
                    return &p;
                }
            }
        }
        return default_part;
    }

    default:
        return nullptr;
    }
}

/* Collect every ACTIVE partition (safe fallback — never false-negative). */
static std::vector<const partition_entry_t *> collect_active_partitions(
        const std::vector<partition_entry_t> &partitions) {
    std::vector<const partition_entry_t *> out;
    out.reserve(partitions.size());
    for (const partition_entry_t &p : partitions) {
        if (p.state == partition_state_t::ACTIVE) {
            out.push_back(&p);
        }
    }
    return out;
}

/* Predicate-based partition selection for the query planner.
 *
 * - UNKNOWN predicate or field mismatch → all ACTIVE (safe fallback)
 * - type NONE → empty
 * - EQUALITY → route() to a single partition (if ACTIVE)
 * - RANGE layout + RANGE pred → O(log P + S) interval overlap
 * - HASH/LIST + RANGE pred → cannot prune (order-agnostic / unordered) */
std::vector<const partition_entry_t *> partition_config_t::prune(
        const partition_predicate_t &pred) const {
    if (type == partition_type_t::NONE) {
        return std::vector<const partition_entry_t *>();
    }

    if (pred.kind == partition_predicate_t::UNKNOWN
            || pred.field != key_field) {
        return collect_active_partitions(partitions);
    }

    switch (pred.kind) {
    case partition_predicate_t::EQUALITY: {
        /* RANGE / HASH / LIST equality all go through route(). */
        const partition_entry_t *p = route(pred.equality_value);
        std::vector<const partition_entry_t *> out;
        if (p != nullptr && p->state == partition_state_t::ACTIVE) {
            out.push_back(p);
        }
        return out;
    }

    case partition_predicate_t::RANGE: {
        /* Hash and list have no datum ordering that maps to partitions. */
        if (type == partition_type_t::HASH || type == partition_type_t::LIST) {
            return collect_active_partitions(partitions);
        }

        /* RANGE layout: partition i owns [boundaries[i], boundaries[i+1]).
         * [lo, hi) overlaps [left, right) iff hi > left && lo < right. */
        std::vector<const partition_entry_t *> out;
        if (type != partition_type_t::RANGE
                || range_boundaries.size() < 2
                || partitions.size() + 1 != range_boundaries.size()) {
            return out;
        }

        const ql::datum_t &lo = pred.range_lo;
        const ql::datum_t &hi = pred.range_hi;
        if (!lo.has() || !hi.has() || !(lo < hi)) {
            /* Empty or invalid predicate range selects nothing. */
            return out;
        }

        /* First boundary strictly greater than lo → right edge of the first
         * partition that can overlap (right > lo). Index of that partition is
         * one before that boundary. */
        auto ub = std::upper_bound(
            range_boundaries.begin(), range_boundaries.end(), lo);
        if (ub == range_boundaries.begin()) {
            /* lo is below the overall domain (before minval). */
            return out;
        }
        size_t first = static_cast<size_t>(
            std::distance(range_boundaries.begin(), ub)) - 1;

        /* Walk consecutive partitions while left < hi. */
        for (size_t i = first; i < partitions.size(); ++i) {
            if (!(range_boundaries[i] < hi)) {
                break;
            }
            if (partitions[i].state == partition_state_t::ACTIVE) {
                out.push_back(&partitions[i]);
            }
        }
        return out;
    }

    case partition_predicate_t::UNKNOWN:
    default:
        return collect_active_partitions(partitions);
    }
}

/* ── partition_map_t ─────────────────────────────────────────────────────── */

std::vector<partition_route_t> partition_map_t::routes_for(
        const partition_selection_t &selection) const {
    std::vector<partition_route_t> out;
    out.reserve(selection.partition_ids.size());
    for (const uuid_u &id : selection.partition_ids) {
        auto it = stores.find(id);
        if (it != stores.end()) {
            out.emplace_back(id, it->second, epoch);
        }
    }
    return out;
}

/* ── public ReQL conversion ──────────────────────────────────────────────── */

static const char *partition_type_to_string(partition_type_t type) {
    switch (type) {
    case partition_type_t::NONE:  return "none";
    case partition_type_t::RANGE: return "range";
    case partition_type_t::HASH:  return "hash";
    case partition_type_t::LIST:  return "list";
    default: unreachable();
    }
}

static const char *partition_state_to_string(partition_state_t state) {
    switch (state) {
    case partition_state_t::CREATING:    return "creating";
    case partition_state_t::CATCHING_UP: return "catching_up";
    case partition_state_t::ACTIVE:      return "active";
    case partition_state_t::DRAINING:    return "draining";
    case partition_state_t::FAILED:      return "failed";
    default: unreachable();
    }
}

ql::datum_t partition_config_to_datum(const partition_config_t &config) {
    if (!config.is_partitioned()) {
        ql::datum_object_builder_t builder;
        builder.overwrite("partitioned", ql::datum_t::boolean(false));
        return std::move(builder).to_datum();
    }

    ql::datum_array_builder_t parts(ql::configured_limits_t::unlimited);
    bool all_active = true;
    for (size_t i = 0; i < config.partitions.size(); ++i) {
        const partition_entry_t &p = config.partitions[i];
        if (p.state != partition_state_t::ACTIVE) {
            all_active = false;
        }
        ql::datum_object_builder_t entry;
        entry.overwrite("name", ql::datum_t(datum_string_t(p.name.str())));
        entry.overwrite("id", ql::datum_t(datum_string_t(uuid_to_str(p.id))));
        entry.overwrite("state",
            ql::datum_t(datum_string_t(partition_state_to_string(p.state))));
        if (config.type == partition_type_t::RANGE
                && config.range_boundaries.size() == config.partitions.size() + 1) {
            entry.overwrite("from", config.range_boundaries[i]);
            entry.overwrite("to", config.range_boundaries[i + 1]);
        }
        if (config.type == partition_type_t::HASH) {
            ql::datum_array_builder_t buckets(ql::configured_limits_t::unlimited);
            for (uint32_t b : p.hash_buckets) {
                buckets.add(ql::datum_t(static_cast<double>(b)));
            }
            entry.overwrite("buckets", std::move(buckets).to_datum());
        }
        if (config.type == partition_type_t::LIST) {
            if (!p.list_values.empty()) {
                ql::datum_array_builder_t values(ql::configured_limits_t::unlimited);
                for (const ql::datum_t &v : p.list_values) {
                    values.add(v);
                }
                entry.overwrite("values", std::move(values).to_datum());
            }
            if (p.list_default) {
                entry.overwrite("default", ql::datum_t::boolean(true));
            }
        }
        parts.add(std::move(entry).to_datum());
    }

    ql::datum_object_builder_t builder;
    builder.overwrite("type",
        ql::datum_t(datum_string_t(partition_type_to_string(config.type))));
    builder.overwrite("by", ql::datum_t(datum_string_t(config.key_field)));
    builder.overwrite("epoch", ql::datum_t(static_cast<double>(config.epoch)));
    builder.overwrite("state",
        ql::datum_t(datum_string_t(all_active ? "ready" : "transitioning")));
    builder.overwrite("partitions", std::move(parts).to_datum());
    if (config.type == partition_type_t::HASH) {
        builder.overwrite("modulus",
            ql::datum_t(static_cast<double>(config.hash_modulus)));
    }
    return std::move(builder).to_datum();
}

