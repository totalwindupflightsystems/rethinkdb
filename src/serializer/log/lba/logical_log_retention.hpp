// Copyright 2026 RethinkDB, all rights reserved.
#ifndef SERIALIZER_LOG_LBA_LOGICAL_LOG_RETENTION_HPP_
#define SERIALIZER_LOG_LBA_LOGICAL_LOG_RETENTION_HPP_

#include <map>
#include <mutex>
#include "containers/uuid.hpp"
#include "rdb_protocol/cdc_types.hpp"

namespace ql {

class logical_log_retention_t {
public:
    void pin_through(const uuid_u &tid, const uuid_u &shid,
                     log_sequence_number_t lsn);
    void advance_slot(const uuid_u &sid, const shard_lsn_t &lsn);
    void release_slot(const uuid_u &sid);
    log_sequence_number_t retention_floor(const uuid_u &tid,
                                          const uuid_u &shid) const;
private:
    mutable std::mutex mutex_;
    struct key_t {
        uuid_u table_id; uuid_u shard_id;
        bool operator<(const key_t &o) const {
            if (table_id < o.table_id) return true;
            if (o.table_id < table_id) return false;
            return shard_id < o.shard_id;
        }
    };
    std::map<key_t, log_sequence_number_t> floors_;
};

}  // namespace ql
#endif
