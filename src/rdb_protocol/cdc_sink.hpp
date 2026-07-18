// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_CDC_SINK_HPP_
#define RDB_PROTOCOL_CDC_SINK_HPP_

#include <cstdint>
#include <string>

#include "containers/name_string.hpp"
#include "containers/uuid.hpp"
#include "rpc/serialize_macros.hpp"
#include "time.hpp"

namespace ql {

enum class cdc_sink_type_t { KAFKA = 0, WEBHOOK = 1, FILE = 2, S3 = 3 };

enum class cdc_sink_state_t {
    CREATING = 0, CONNECTING = 1, STREAMING = 2,
    DROPPING = 3, DROPPED = 4, ERROR = 5
};

struct cdc_sink_config_t {
    uuid_u sink_id;
    name_string_t name;
    uuid_u publication_id;
    cdc_sink_type_t sink_type = cdc_sink_type_t::KAFKA;
    std::string connection_string;
    std::string credential_ref;
    name_string_t topic;
    cdc_sink_state_t state = cdc_sink_state_t::CREATING;
    uuid_u created_by_user_id;
    microtime_t created_at;
};

struct cdc_batching_config_t {
    uint64_t max_records = 1000;
    uint64_t max_in_flight_batches = 5;
    uint64_t flush_interval_ms = 250;
    uint64_t max_buffer_bytes = 16ULL * 1024 * 1024;
};

}  // namespace ql

ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    ql::cdc_sink_type_t, int8_t,
    ql::cdc_sink_type_t::KAFKA, ql::cdc_sink_type_t::S3);
ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    ql::cdc_sink_state_t, int8_t,
    ql::cdc_sink_state_t::CREATING, ql::cdc_sink_state_t::ERROR);
RDB_DECLARE_SERIALIZABLE(ql::cdc_sink_config_t);
RDB_DECLARE_SERIALIZABLE(ql::cdc_batching_config_t);

#endif  // RDB_PROTOCOL_CDC_SINK_HPP_
