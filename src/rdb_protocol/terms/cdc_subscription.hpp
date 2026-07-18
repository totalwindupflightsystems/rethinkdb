// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_TERMS_CDC_SUBSCRIPTION_HPP_
#define RDB_PROTOCOL_TERMS_CDC_SUBSCRIPTION_HPP_

#include <string>

#include "containers/name_string.hpp"
#include "rdb_protocol/publication.hpp"
#include "rdb_protocol/subscription.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/error.hpp"

namespace ql {

/* Intermediate parse result for subscription create. UUIDs for target
 * database/table are resolved later when the backend is wired (CDC-06). */
struct parsed_subscription_create_t {
    name_string_t name;
    name_string_t publication_name;
    std::string source;  // optional source locator string
    std::string target_db;
    std::string target_table;
    conflict_resolution_t conflict_policy = conflict_resolution_t::LAST_WRITE_WINS;
    snapshot_mode_t snapshot_mode = snapshot_mode_t::FULL;
    uint32_t apply_batch_size = 1000;
    // auth/tls are required by the design; validated as present objects/refs
    // but not resolved until CDC-06.
    bool has_auth = false;
    bool has_tls = false;
};

parsed_subscription_create_t parse_subscription_config_from_datum(
    const datum_t &d, rcheckable_t *target);

}  // namespace ql

#endif  // RDB_PROTOCOL_TERMS_CDC_SUBSCRIPTION_HPP_
