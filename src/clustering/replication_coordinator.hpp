// Copyright 2026 RethinkDB, all rights reserved.
#ifndef CLUSTERING_REPLICATION_COORDINATOR_HPP_
#define CLUSTERING_REPLICATION_COORDINATOR_HPP_

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "containers/uuid.hpp"
#include "perfmon/perfmon.hpp"
#include "rdb_protocol/cdc_types.hpp"
#include "rdb_protocol/publication.hpp"
#include "serializer/log/lba/logical_log_retention.hpp"

namespace ql {

struct replication_consumer_identity_t {
    uuid_u consumer_id;
    std::string credential_ref;
    std::vector<std::string> supported_schemas;
};

enum class replication_slot_state_t {
    INITIALIZING, SNAPSHOTTING, STREAMING, PAUSED,
    RETRYING, RESYNC_REQUIRED, EVICTED, ERROR, DROPPING
};

struct replication_slot_info_t {
    uuid_u slot_id;
    uuid_u publication_id;
    replication_consumer_identity_t consumer;
    replication_slot_state_t state = replication_slot_state_t::INITIALIZING;
    std::map<uuid_u, log_sequence_number_t> confirmed_lsn_by_shard;
    std::map<uuid_u, log_sequence_number_t> flush_lsn_by_shard;
    std::map<uuid_u, uint64_t> shard_incarnation_by_shard;
    uint64_t retained_bytes = 0;
};

struct slot_backpressure_t {
    uint64_t in_flight_batches = 0;
    uint64_t buffered_bytes = 0;
    bool source_paused = false;
};

struct slot_lag_t {
    uint64_t lag_bytes = 0;
    uint64_t lag_lsn = 0;
    uint64_t lag_ms = 0;
    bool warn_threshold_breached = false;
    bool hard_threshold_breached = false;
};

class replication_coordinator_t {
public:
    explicit replication_coordinator_t(logical_log_retention_t *r);

    // Slot lifecycle
    void create_slot(const uuid_u &sid, const uuid_u &pid,
                     const replication_consumer_identity_t &c);
    void bind_slot(const uuid_u &sid, const publication_config_t &pub,
                   const std::map<uuid_u, log_sequence_number_t> &base);
    std::optional<replication_slot_info_t> get_slot_state(const uuid_u &sid) const;
    void advance_slot(const uuid_u &sid, const shard_lsn_t &lsn, uint64_t inc);
    bool note_flush_lsn(const uuid_u &sid, const shard_lsn_t &lsn, uint64_t inc);
    bool confirm_lsn(const uuid_u &sid, const shard_lsn_t &lsn, uint64_t inc);
    void pause_slot(const uuid_u &sid);
    void resume_slot(const uuid_u &sid);
    void drop_slot(const uuid_u &sid);
    void evict_slot(const uuid_u &sid, const std::string &why,
                    const std::string &detail);

    // Backpressure
    void configure_backpressure(const uuid_u &sid, uint64_t maxf, uint64_t maxb);
    bool on_batch_enqueued(const uuid_u &sid, uint64_t b);
    void on_batch_dequeued(const uuid_u &sid, uint64_t b);
    slot_backpressure_t get_backpressure(const uuid_u &sid) const;

    // Lag
    void note_journal_high_water(const uuid_u &tid, const uuid_u &shid,
                                 log_sequence_number_t lsn,
                                 uint64_t b, uint64_t ms);
    std::optional<slot_lag_t> get_slot_lag(const uuid_u &sid) const;

    // Routing
    void on_shard_routing_change(const uuid_u &pid,
                                 std::tuple<uuid_u, uint64_t, uint64_t> ch);

    // Observability (CDC-08f) — low-cardinality counters, names are the only
    // labels. Never inject user data / table names / URLs as labels.
    void record_captured(uint64_t count = 1);
    void record_delivered(uint64_t count = 1);
    void record_delivery_latency(uint64_t ms);
    void record_sink_retry();
    void record_sink_dead_letter();
    void record_resync_required();

private:
    perfmon_collection_t cdc_perfmon_collection;
    perfmon_membership_t cdc_perfmon_membership;
    perfmon_membership_t cdc_records_captured_membership;
    perfmon_membership_t cdc_records_delivered_membership;
    perfmon_membership_t cdc_delivery_latency_membership;
    perfmon_membership_t cdc_slot_lag_bytes_membership;
    perfmon_membership_t cdc_slot_lag_lsn_membership;
    perfmon_membership_t cdc_retained_journal_bytes_membership;
    perfmon_membership_t cdc_sink_retries_membership;
    perfmon_membership_t cdc_sink_dead_letter_membership;
    perfmon_membership_t cdc_resync_required_membership;

    perfmon_counter_t cdc_records_captured_total;
    perfmon_counter_t cdc_records_delivered_total;
    perfmon_counter_t cdc_delivery_latency_ms;
    mutable perfmon_counter_t cdc_slot_lag_bytes;
    mutable perfmon_counter_t cdc_slot_lag_lsn;
    perfmon_counter_t cdc_retained_journal_bytes;
    perfmon_counter_t cdc_sink_retries_total;
    perfmon_counter_t cdc_sink_dead_letter_total;
    perfmon_counter_t cdc_resync_required_total;

    logical_log_retention_t *retention_;
    mutable std::mutex mutex_;
    std::map<uuid_u, replication_slot_info_t> slots_;
    std::map<uuid_u, publication_config_t> publications_;
    std::map<uuid_u, std::pair<uint64_t, uint64_t>> bp_cfg_;
    std::map<uuid_u, slot_backpressure_t> bp_state_;
    struct hw_t { log_sequence_number_t lsn; uint64_t bytes; uint64_t ms; };
    std::map<std::pair<uuid_u, uuid_u>, hw_t> journal_hw_;
};

}  // namespace ql

#endif
