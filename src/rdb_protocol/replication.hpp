// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_REPLICATION_HPP_
#define RDB_PROTOCOL_REPLICATION_HPP_

#include <cstdint>
#include <set>

#include "containers/name_string.hpp"
#include "containers/uuid.hpp"
#include "rdb_protocol/cdc_types.hpp"
#include "rpc/serialize_macros.hpp"
#include "time.hpp"

namespace ql {

enum class replication_slot_kind_t { SUBSCRIPTION = 0, CDC_SINK = 1 };
enum class replication_slot_state_t { ACTIVE = 0, PAUSED = 1, ERROR = 2, EVICTED = 3 };

struct replication_slot_t {
    uuid_u slot_id;
    name_string_t name;
    uuid_u publication_id;
    replication_slot_kind_t kind = replication_slot_kind_t::SUBSCRIPTION;
    replication_slot_state_t state = replication_slot_state_t::ACTIVE;
    shard_lsn_t confirmed_lsn;
    microtime_t last_heartbeat;
};

struct applied_change_t {
    change_event_id_t event_id;
    microtime_t applied_at;
};

// subscription_applier_t lives in subscription.hpp (spec §3.7).
// The old placeholder struct was removed in CDC-06e.

}  // namespace ql

ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    ql::replication_slot_kind_t, int8_t,
    ql::replication_slot_kind_t::SUBSCRIPTION, ql::replication_slot_kind_t::CDC_SINK);
ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    ql::replication_slot_state_t, int8_t,
    ql::replication_slot_state_t::ACTIVE, ql::replication_slot_state_t::EVICTED);
RDB_DECLARE_SERIALIZABLE(ql::replication_slot_t);
RDB_DECLARE_SERIALIZABLE(ql::applied_change_t);

#endif  // RDB_PROTOCOL_REPLICATION_HPP_
