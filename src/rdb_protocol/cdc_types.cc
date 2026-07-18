// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "rdb_protocol/cdc_types.hpp"

#include "containers/archive/archive.hpp"
#include "containers/archive/stl_types.hpp"
#include "containers/archive/versioned.hpp"

RDB_IMPL_SERIALIZABLE_1_SINCE_v2_4(ql::log_sequence_number_t, value);
RDB_IMPL_SERIALIZABLE_2_SINCE_v2_4(ql::shard_lsn_t, shard_id, lsn);
RDB_IMPL_SERIALIZABLE_4_SINCE_v2_4(ql::change_event_id_t,
    source_cluster_id, table_id, shard_id, lsn);
RDB_IMPL_SERIALIZABLE_5_SINCE_v2_4(ql::change_record_t,
    event_id, op, before_image, after_image, commit_timestamp);
