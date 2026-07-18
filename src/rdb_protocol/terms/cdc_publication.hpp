// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_TERMS_CDC_PUBLICATION_HPP_
#define RDB_PROTOCOL_TERMS_CDC_PUBLICATION_HPP_

#include "rdb_protocol/publication.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/error.hpp"

namespace ql {

/* Parse and validate a publication config object from ReQL. Assigns a fresh
 * publication_id and CREATING state. Throws via rcheck_target / rfail_target
 * on invalid shapes. Backend wiring is deferred to CDC-05. */
publication_config_t parse_publication_config_from_datum(
    const datum_t &d, rcheckable_t *target);

/* Cluster-version gate for CDC DDL. Rejects until every connected member can
 * deserialize CDC metadata. Stub until a dedicated cluster version lands. */
void require_cdc_cluster_support(const rcheckable_t *target);

}  // namespace ql

#endif  // RDB_PROTOCOL_TERMS_CDC_PUBLICATION_HPP_
