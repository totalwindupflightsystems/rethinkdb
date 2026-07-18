// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/subscription.hpp"

#include "containers/archive/archive.hpp"
#include "containers/archive/stl_types.hpp"
#include "containers/archive/versioned.hpp"

RDB_IMPL_SERIALIZABLE_10_SINCE_v2_4(
    ql::subscription_config_t,
    subscription_id, name, target_database_id, target_table_id,
    publication_name, source_cluster_id, conflict_policy, state,
    created_by_user_id, created_at);
