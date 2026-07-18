// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_PUBLICATION_HPP_
#define RDB_PROTOCOL_PUBLICATION_HPP_

#include <cstdint>
#include <map>
#include <set>
#include <string>

#include "containers/name_string.hpp"
#include "containers/uuid.hpp"
#include "rdb_protocol/cdc_types.hpp"
#include "rpc/serialize_macros.hpp"
#include "time.hpp"

namespace ql {

enum class publication_format_t { JSON_V1 = 0, INTERNAL_RDB_V1 = 1 };

enum class publication_state_t { CREATING = 0, READY = 1, DROPPING = 2, DROPPED = 3, ERROR = 4 };

enum class snapshot_mode_t { FULL = 0, NONE = 1 };

struct publication_filter_t {
    std::set<std::string> projected_fields;
    // operations bitmap serialized separately in CDC-02 term implementation
    uint32_t allowed_operations = 0x0F;  // all operations by default

    bool operator==(const publication_filter_t &other) const {
        return projected_fields == other.projected_fields
            && allowed_operations == other.allowed_operations;
    }
    bool operator!=(const publication_filter_t &other) const {
        return !(*this == other);
    }
};

struct publication_config_t {
    uuid_u publication_id;
    name_string_t name;
    uuid_u database_id;
    uuid_u table_id;
    publication_filter_t filter;
    publication_format_t format = publication_format_t::JSON_V1;
    bool include_before_image = true;
    bool include_after_image = true;
    snapshot_mode_t default_snapshot_mode = snapshot_mode_t::FULL;
    uint64_t max_slot_lag_bytes = 1024ULL * 1024 * 1024;
    publication_state_t state = publication_state_t::CREATING;
    uuid_u created_by_user_id;
    microtime_t created_at;

    bool operator==(const publication_config_t &other) const {
        return publication_id == other.publication_id
            && name == other.name
            && database_id == other.database_id
            && table_id == other.table_id
            && filter == other.filter
            && format == other.format
            && include_before_image == other.include_before_image
            && include_after_image == other.include_after_image
            && default_snapshot_mode == other.default_snapshot_mode
            && max_slot_lag_bytes == other.max_slot_lag_bytes
            && state == other.state
            && created_by_user_id == other.created_by_user_id
            && created_at == other.created_at;
    }
    bool operator!=(const publication_config_t &other) const {
        return !(*this == other);
    }
};

}  // namespace ql

ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    ql::publication_format_t, int8_t,
    ql::publication_format_t::JSON_V1, ql::publication_format_t::INTERNAL_RDB_V1);
ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    ql::publication_state_t, int8_t,
    ql::publication_state_t::CREATING, ql::publication_state_t::ERROR);
ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    ql::snapshot_mode_t, int8_t,
    ql::snapshot_mode_t::FULL, ql::snapshot_mode_t::NONE);
RDB_DECLARE_SERIALIZABLE(ql::publication_filter_t);
RDB_DECLARE_SERIALIZABLE(ql::publication_config_t);

#endif  // RDB_PROTOCOL_PUBLICATION_HPP_
