// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/replication.hpp"

#include "containers/archive/archive.hpp"
#include "containers/archive/stl_types.hpp"
#include "containers/archive/versioned.hpp"

RDB_IMPL_SERIALIZABLE_7_SINCE_v2_4(
    ql::replication_slot_t,
    slot_id, name, publication_id, kind, state, confirmed_lsn, last_heartbeat);
RDB_IMPL_SERIALIZABLE_2_SINCE_v2_4(
    ql::applied_change_t, event_id, applied_at);
