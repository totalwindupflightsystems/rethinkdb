// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/cdc_sink.hpp"

#include "containers/archive/archive.hpp"
#include "containers/archive/stl_types.hpp"
#include "containers/archive/versioned.hpp"

RDB_IMPL_SERIALIZABLE_10_SINCE_v2_4(
    ql::cdc_sink_config_t,
    sink_id, name, publication_id, sink_type, connection_string,
    credential_ref, topic, state, created_by_user_id, created_at);
RDB_IMPL_SERIALIZABLE_4_SINCE_v2_4(
    ql::cdc_batching_config_t,
    max_records, max_in_flight_batches, flush_interval_ms, max_buffer_bytes);
