// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_TERMS_PARTITIONING_HPP_
#define RDB_PROTOCOL_TERMS_PARTITIONING_HPP_

#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/error.hpp"
#include "rdb_protocol/partition_config.hpp"

namespace ql {

/* Parse a ReQL `partitions` optarg / `repartition` config object into a
 * partition_config_t. Assigns fresh partition UUIDs and ACTIVE state. Throws
 * via rfail_target on invalid shapes. */
partition_config_t parse_partition_config_from_datum(
    const datum_t &d, rcheckable_t *target);

}  // namespace ql

#endif  // RDB_PROTOCOL_TERMS_PARTITIONING_HPP_
