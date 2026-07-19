// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_SUBSCRIPTION_HPP_
#define RDB_PROTOCOL_SUBSCRIPTION_HPP_

#include <cstdint>
#include <string>

#include "containers/name_string.hpp"
#include "containers/uuid.hpp"
#include "rpc/serialize_macros.hpp"
#include "time.hpp"

namespace ql {

class datum_t;

enum class subscription_state_t {
    CREATING = 0, CONNECTING = 1, SNAPSHOTTING = 2,
    CATCHING_UP = 3, STREAMING = 4, DROPPING = 5,
    DROPPED = 6, ERROR = 7
};

enum class conflict_resolution_t {
    LAST_WRITE_WINS = 0, PRIMARY_KEY_MERGE = 1, CUSTOM_HANDLER = 2
};

struct subscription_config_t {
    uuid_u subscription_id;
    name_string_t name;
    uuid_u target_database_id;
    uuid_u target_table_id;
    name_string_t publication_name;
    uuid_u source_cluster_id;
    conflict_resolution_t conflict_policy = conflict_resolution_t::LAST_WRITE_WINS;
    subscription_state_t state = subscription_state_t::CREATING;
    uuid_u created_by_user_id;
    microtime_t created_at;

    bool operator==(const subscription_config_t &other) const {
        return subscription_id == other.subscription_id
            && name == other.name
            && target_database_id == other.target_database_id
            && target_table_id == other.target_table_id
            && publication_name == other.publication_name
            && source_cluster_id == other.source_cluster_id
            && conflict_policy == other.conflict_policy
            && state == other.state
            && created_by_user_id == other.created_by_user_id
            && created_at == other.created_at;
    }
    bool operator!=(const subscription_config_t &other) const {
        return !(*this == other);
    }
};

/* CDC-06b: inline helpers for serializing subscription config to ReQL datums.
   Follows the same pattern as publication_state_to_string and
   publication_config_to_datum in publication.hpp / cdc_publication.cc. */

inline const char *subscription_state_to_string(subscription_state_t state) {
    switch (state) {
    case subscription_state_t::CREATING:     return "creating";
    case subscription_state_t::CONNECTING:   return "connecting";
    case subscription_state_t::SNAPSHOTTING: return "snapshotting";
    case subscription_state_t::CATCHING_UP:  return "catching_up";
    case subscription_state_t::STREAMING:    return "streaming";
    case subscription_state_t::DROPPING:     return "dropping";
    case subscription_state_t::DROPPED:      return "dropped";
    case subscription_state_t::ERROR:        return "error";
    default:                                 return "unknown";
    }
}

inline const char *conflict_resolution_to_string(conflict_resolution_t policy) {
    switch (policy) {
    case conflict_resolution_t::LAST_WRITE_WINS:   return "last_write_wins";
    case conflict_resolution_t::PRIMARY_KEY_MERGE: return "primary_key_merge";
    case conflict_resolution_t::CUSTOM_HANDLER:    return "custom_handler";
    default:                                       return "unknown";
    }
}

// Defined in terms/cdc_subscription.cc — avoids pulling datum headers
// into every compilation unit that includes this header.
datum_t subscription_config_to_datum(
        const subscription_config_t &config);

}  // namespace ql

ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    ql::subscription_state_t, int8_t,
    ql::subscription_state_t::CREATING, ql::subscription_state_t::ERROR);
ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    ql::conflict_resolution_t, int8_t,
    ql::conflict_resolution_t::LAST_WRITE_WINS, ql::conflict_resolution_t::CUSTOM_HANDLER);
RDB_DECLARE_SERIALIZABLE(ql::subscription_config_t);

#endif  // RDB_PROTOCOL_SUBSCRIPTION_HPP_
