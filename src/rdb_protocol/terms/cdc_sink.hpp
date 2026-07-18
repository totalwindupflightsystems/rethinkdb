// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_TERMS_CDC_SINK_HPP_
#define RDB_PROTOCOL_TERMS_CDC_SINK_HPP_

#include <string>
#include <vector>

#include "containers/name_string.hpp"
#include "rdb_protocol/cdc_sink.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/error.hpp"

namespace ql {

/* Intermediate parse result for CDC sink create. */
struct parsed_cdc_sink_create_t {
    name_string_t name;
    name_string_t publication_name;
    cdc_sink_type_t sink_type = cdc_sink_type_t::KAFKA;
    std::string connection_string;  // brokers joined, url, or bucket
    std::string credential_ref;
    std::string topic;  // kafka topic / s3 prefix / label (not name_string — may contain '.')
    cdc_batching_config_t batching;
};

parsed_cdc_sink_create_t parse_cdc_sink_config_from_datum(
    const datum_t &d, rcheckable_t *target);

}  // namespace ql

#endif  // RDB_PROTOCOL_TERMS_CDC_SINK_HPP_
