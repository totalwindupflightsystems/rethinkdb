// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/publication.hpp"

#include "containers/archive/archive.hpp"
#include "containers/archive/stl_types.hpp"
#include "containers/archive/versioned.hpp"

RDB_IMPL_SERIALIZABLE_2_SINCE_v2_4(
    ql::publication_filter_t, projected_fields, allowed_operations);
RDB_IMPL_SERIALIZABLE_13_SINCE_v2_4(
    ql::publication_config_t,
    publication_id, name, database_id, table_id, filter, format,
    include_before_image, include_after_image, default_snapshot_mode,
    max_slot_lag_bytes, state, created_by_user_id, created_at);
