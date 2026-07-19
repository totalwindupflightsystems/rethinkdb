// Copyright 2026 RethinkDB, all rights reserved.
#ifndef CLUSTERING_REPLICATION_COORDINATOR_HPP_
#define CLUSTERING_REPLICATION_COORDINATOR_HPP_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "concurrency/auto_drainer.hpp"
#include "containers/optional.hpp"
#include "containers/uuid.hpp"
#include "rdb_protocol/cdc_types.hpp"
#include "rdb_protocol/publication.hpp"
#include "rpc/mailbox/typed.hpp"
#include "rpc/serialize_macros.hpp"
#include "threading.hpp"
#include "time.hpp"

namespace ql {

constexpr uint32_t CDC_REPLICATION_PROTOCOL_VERSION = 1;
constexpr uint64_t CDC_DEFAULT_MAX_IN_FLIGHT_BATCHES = 4;
constexpr uint64_t CDC_DEFAULT_MAX_BUFFER_BYTES = 16ULL * 1024 * 1024;

enum class replication_slot_state_t : uint8_t {
    INITIALIZING = 0,
    CREATING = 1,
    SNAPSHOTTING = 2,
    CONNECTING = 3,
    CATCHING_UP = 4,
    STREAMING = 5,
    PAUSED = 6,
    RETRYING = 7,
    RESYNC_REQUIRED = 8,
    EVICTED = 9,
    ERROR = 10,
    DROPPED = 11,
    DROPPING = 12
};

struct replication_consumer_identity_t {
    uuid_u consumer_id;
    std::string credential_ref;
    std::vector<std::string> supported_schemas;
    uint32_t protocol_version = CDC_REPLICATION_PROTOCOL_VERSION;

    bool operator==(const replication_consumer_identity_t &other) const {
        return consumer_id == other.consumer_id
            && credential_ref == other.credential_ref
            && supported_schemas == other.supported_schemas
            && protocol_version == other.protocol_version;
    }
    bool operator!=(const replication_consumer_identity_t &other) const {
        return !(*this == other);
    }
};

struct replication_slot_record_t {
    uuid_u slot_id;
    uuid_u publication_id;
    uuid_u table_id;
    replication_consumer_identity_t consumer;
    replication_slot_state_t state = replication_slot_state_t::INITIALIZING;
    std::map<uuid_u, log_sequence_number_t> confirmed_lsn_by_shard;
    std::map<uuid_u, log_sequence_number_t> flush_lsn_by_shard;
    std::map<uuid_u, log_sequence_number_t> snapshot_barrier_lsn_by_shard;
    std::map<uuid_u, uint64_t> shard_incarnation_by_shard;
    microtime_t last_ack_at = 0;
    uint64_t retained_bytes = 0;
    uint64_t lag_bytes = 0;
    uint64_t lag_lsn = 0;
    microtime_t lag_ms = 0;
    uint64_t lag_warn_bytes = 0;
    uint64_t lag_hard_bytes = 0;
    std::string last_error_code;
    std::string last_error_message;
    microtime_t last_error_at = 0;
    microtime_t created_at = 0;
    std::map<uuid_u, log_sequence_number_t> eviction_floor_by_shard;

    bool operator==(const replication_slot_record_t &other) const {
        return slot_id == other.slot_id
            && publication_id == other.publication_id
            && table_id == other.table_id
            && consumer == other.consumer
            && state == other.state
            && confirmed_lsn_by_shard == other.confirmed_lsn_by_shard
            && flush_lsn_by_shard == other.flush_lsn_by_shard
            && snapshot_barrier_lsn_by_shard
                == other.snapshot_barrier_lsn_by_shard
            && shard_incarnation_by_shard == other.shard_incarnation_by_shard
            && last_ack_at == other.last_ack_at
            && retained_bytes == other.retained_bytes
            && lag_bytes == other.lag_bytes
            && lag_lsn == other.lag_lsn
            && lag_ms == other.lag_ms
            && lag_warn_bytes == other.lag_warn_bytes
            && lag_hard_bytes == other.lag_hard_bytes
            && last_error_code == other.last_error_code
            && last_error_message == other.last_error_message
            && last_error_at == other.last_error_at
            && created_at == other.created_at
            && eviction_floor_by_shard == other.eviction_floor_by_shard;
    }
    bool operator!=(const replication_slot_record_t &other) const {
        return !(*this == other);
    }
};

struct slot_backpressure_t {
    uint64_t in_flight_batches = 0;
    uint64_t buffered_bytes = 0;
    uint64_t max_in_flight_batches = CDC_DEFAULT_MAX_IN_FLIGHT_BATCHES;
    uint64_t max_buffer_bytes = CDC_DEFAULT_MAX_BUFFER_BYTES;
    bool source_paused = false;

    bool at_capacity() const {
        return in_flight_batches >= max_in_flight_batches
            || buffered_bytes >= max_buffer_bytes;
    }
};

struct slot_lag_t {
    uint64_t lag_bytes = 0;
    uint64_t lag_lsn = 0;
    microtime_t lag_ms = 0;
    bool warn_threshold_breached = false;
    bool hard_threshold_breached = false;
};

struct shard_routing_event_t {
    uuid_u shard_id;
    uint64_t new_incarnation = 0;
    microtime_t observed_at = 0;
};

struct shard_journal_high_water_t {
    log_sequence_number_t lsn;
    uint64_t retained_bytes = 0;
    microtime_t commit_timestamp = 0;
};

struct cdc_observability_t {
    uint64_t cdc_records_captured_total = 0;
    uint64_t cdc_records_delivered_total = 0;
    microtime_t cdc_delivery_latency_ms = 0;
    uint64_t cdc_retained_journal_bytes = 0;
    uint64_t cdc_sink_retries_total = 0;
    uint64_t cdc_sink_dead_letter_total = 0;
    uint64_t cdc_resync_required_total = 0;
    std::map<uuid_u, uint64_t> cdc_slot_lag_bytes;
    std::map<uuid_u, uint64_t> cdc_slot_lag_lsn;
};

struct replication_coordinator_business_card_t {
    typedef mailbox_t<uuid_u, shard_lsn_t, uint64_t> confirm_mailbox_t;
    typedef mailbox_t<uuid_u> control_mailbox_t;

    confirm_mailbox_t::address_t confirm_mailbox;
    control_mailbox_t::address_t control_mailbox;

    bool operator==(const replication_coordinator_business_card_t &other) const {
        return confirm_mailbox == other.confirm_mailbox
            && control_mailbox == other.control_mailbox;
    }
    bool operator!=(const replication_coordinator_business_card_t &other) const {
        return !(*this == other);
    }

    RDB_MAKE_ME_SERIALIZABLE_2(
        replication_coordinator_business_card_t,
        confirm_mailbox,
        control_mailbox);
};

class slot_lifecycle_t {
public:
    virtual ~slot_lifecycle_t() = default;

    virtual void create_slot(
        const uuid_u &slot_id,
        const uuid_u &publication_id,
        const replication_consumer_identity_t &consumer) = 0;
    virtual void bind_slot(
        const uuid_u &slot_id,
        const publication_config_t &publication,
        const std::map<uuid_u, log_sequence_number_t> &snapshot_barriers) = 0;
    virtual bool confirm_lsn(
        const uuid_u &slot_id,
        const shard_lsn_t &confirmed,
        uint64_t incarnation) = 0;
    virtual void pause_slot(const uuid_u &slot_id) = 0;
    virtual void resume_slot(const uuid_u &slot_id) = 0;
    virtual void evict_slot(
        const uuid_u &slot_id,
        const std::string &code,
        const std::string &message) = 0;
    virtual void drop_slot(const uuid_u &slot_id) = 0;
};

class logical_log_retention_t {
public:
    logical_log_retention_t() = default;

    void pin_through(
        const uuid_u &table_id,
        const uuid_u &shard_id,
        const uuid_u &slot_id,
        log_sequence_number_t required_lsn);
    bool advance_slot(
        const uuid_u &slot_id,
        const shard_lsn_t &confirmed);
    void release_slot(const uuid_u &slot_id);
    log_sequence_number_t retention_floor(
        const uuid_u &table_id,
        const uuid_u &shard_id) const;
    bool can_reclaim_extent(
        const uuid_u &table_id,
        const uuid_u &shard_id,
        log_sequence_number_t extent_last_lsn) const;
    std::vector<uuid_u> blocking_slots(
        const uuid_u &table_id,
        const uuid_u &shard_id) const;

private:
    struct pin_key_t {
        uuid_u slot_id;
        uuid_u table_id;
        uuid_u shard_id;

        bool operator<(const pin_key_t &other) const {
            if (slot_id < other.slot_id) return true;
            if (other.slot_id < slot_id) return false;
            if (table_id < other.table_id) return true;
            if (other.table_id < table_id) return false;
            return shard_id < other.shard_id;
        }
    };

    mutable std::mutex mutex_;
    std::map<pin_key_t, log_sequence_number_t> pins_;

    DISABLE_COPYING(logical_log_retention_t);
};

class replication_coordinator_t : public home_thread_mixin_t,
                                  public slot_lifecycle_t {
public:
    explicit replication_coordinator_t(logical_log_retention_t *retention);
    replication_coordinator_t(
        mailbox_manager_t *mailbox_manager,
        logical_log_retention_t *retention);
    ~replication_coordinator_t();

    replication_coordinator_business_card_t get_business_card() const;

    void create_slot(
        const uuid_u &slot_id,
        const uuid_u &publication_id,
        const replication_consumer_identity_t &consumer) override;
    void bind_slot(
        const uuid_u &slot_id,
        const publication_config_t &publication,
        const std::map<uuid_u, log_sequence_number_t> &snapshot_barriers) override;
    bool confirm_lsn(
        const uuid_u &slot_id,
        const shard_lsn_t &confirmed,
        uint64_t incarnation) override;
    bool note_flush_lsn(
        const uuid_u &slot_id,
        const shard_lsn_t &flush_lsn,
        uint64_t incarnation);
    void advance_slot(
        const uuid_u &slot_id,
        const shard_lsn_t &confirmed,
        uint64_t incarnation);
    void pause_slot(const uuid_u &slot_id) override;
    void resume_slot(const uuid_u &slot_id) override;
    void evict_slot(
        const uuid_u &slot_id,
        const std::string &code,
        const std::string &message) override;
    void drop_slot(const uuid_u &slot_id) override;
    void mark_resync_required(
        const uuid_u &slot_id,
        const std::string &code,
        const std::string &message);

    optional<replication_slot_record_t> get_slot_state(
        const uuid_u &slot_id) const;
    std::vector<replication_slot_record_t> list_slots() const;
    size_t streaming_slot_count() const;

    void configure_backpressure(
        const uuid_u &slot_id,
        uint64_t max_in_flight_batches,
        uint64_t max_buffer_bytes);
    bool on_batch_enqueued(const uuid_u &slot_id, uint64_t bytes);
    void on_batch_dequeued(const uuid_u &slot_id, uint64_t bytes);
    slot_backpressure_t get_backpressure(const uuid_u &slot_id) const;

    void note_journal_high_water(
        const uuid_u &table_id,
        const uuid_u &shard_id,
        log_sequence_number_t high_water_lsn,
        uint64_t retained_bytes,
        microtime_t commit_timestamp);
    optional<slot_lag_t> get_slot_lag(const uuid_u &slot_id) const;
    uint64_t total_retained_bytes() const;
    std::vector<uuid_u> slots_blocking_reclamation(
        const uuid_u &table_id,
        const uuid_u &shard_id) const;

    void on_shard_routing_change(
        const uuid_u &publication_id,
        const shard_routing_event_t &event);

    void record_slot_error(
        const uuid_u &slot_id,
        const std::string &code,
        const std::string &message);

    void note_records_captured(uint64_t count);
    void note_records_delivered(uint64_t count, microtime_t latency_ms);
    void note_sink_retry();
    void note_sink_dead_letter();
    cdc_observability_t observability() const;

private:
    void initialize_mailboxes(mailbox_manager_t *mailbox_manager);
    void on_confirm_mailbox(
        signal_t *interruptor,
        uuid_u slot_id,
        shard_lsn_t confirmed,
        uint64_t incarnation);
    void on_control_mailbox(signal_t *interruptor, uuid_u slot_id);

    bool transition_allowed(
        replication_slot_state_t from,
        replication_slot_state_t to) const;
    void transition_locked(
        const uuid_u &slot_id,
        replication_slot_state_t to);
    void recompute_lag_locked(
        replication_slot_record_t *slot);
    void release_retention_locked(const uuid_u &slot_id);

    mutable std::mutex mutex_;
    logical_log_retention_t *retention_;
    std::map<uuid_u, replication_slot_record_t> slots_;
    std::map<uuid_u, slot_backpressure_t> backpressure_;
    std::map<std::pair<uuid_u, uuid_u>, shard_journal_high_water_t>
        journal_high_water_;
    cdc_observability_t observability_;

    mailbox_manager_t *mailbox_manager_ = nullptr;
    optional<mailbox_t<uuid_u, shard_lsn_t, uint64_t>> confirm_mailbox_;
    optional<mailbox_t<uuid_u>> control_mailbox_;
    auto_drainer_t drainer_;

    DISABLE_COPYING(replication_coordinator_t);
};

}  // namespace ql

ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    ql::replication_slot_state_t,
    uint8_t,
    ql::replication_slot_state_t::INITIALIZING,
    ql::replication_slot_state_t::DROPPING);

#endif  // CLUSTERING_REPLICATION_COORDINATOR_HPP_
