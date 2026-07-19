// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_CDC_SINK_HPP_
#define RDB_PROTOCOL_CDC_SINK_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "concurrency/signal.hpp"
#include "containers/name_string.hpp"
#include "containers/uuid.hpp"
#include "rdb_protocol/cdc_types.hpp"
#include "rdb_protocol/publication.hpp"
#include "rpc/serialize_macros.hpp"
#include "threading.hpp"
#include "time.hpp"

namespace ql {

// ── Sink metadata (durable configuration) ──────────────────────────────────

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

// Batching knobs for the dispatcher (spec §5.5). Defaults match the existing
// durable config shape; the coordinator enforces bounds before dispatch.
struct cdc_batching_config_t {
    uint64_t max_records = 1000;
    uint64_t max_in_flight_batches = 5;
    uint64_t flush_interval_ms = 250;
    uint64_t max_buffer_bytes = 16ULL * 1024 * 1024;
};

// ── Delivery results (spec §5.6, §6.5) ─────────────────────────────────────

// Contiguous durable acknowledgement or a typed failure. Drivers must never
// report OK when the external write was not durably accepted.
enum class sink_delivery_outcome_t {
    OK = 0,
    RETRYABLE_ERROR = 1,
    PERMANENT_ERROR = 2
};

struct sink_delivery_result_t {
    sink_delivery_outcome_t outcome = sink_delivery_outcome_t::OK;
    // Highest contiguous LSN acknowledged for `shard_id` after a durable
    // external write (or durable DLQ write). Zero/nil when nothing advanced.
    uuid_u shard_id;
    log_sequence_number_t acknowledged_lsn;
    std::string error_message;
    // True when the original batch was parked on the dead-letter path and the
    // slot may advance past the DLQ'd range (spec §8.4).
    bool dead_lettered = false;
};

// ── Dead-letter queue (spec §8.4) ──────────────────────────────────────────

// Optional DLQ configuration attached to a sink. Credentials remain secret
// references; never literal durable fields.
struct dead_letter_config_t {
    cdc_sink_type_t type = cdc_sink_type_t::FILE;
    std::string connection_string;  // topic / url / path / bucket
    std::string topic_or_prefix;    // kafka topic, s3 prefix, or label
    std::string credential_ref;
};

// Durable DLQ event: original envelope + redacted diagnostic. The slot
// advances only after this record is written durably.
struct dead_letter_record_t {
    uuid_u sink_id;
    uuid_u slot_id;
    uuid_u publication_id;
    change_event_id_t event_id;
    change_operation_t op = change_operation_t::INSERT;
    std::vector<char> before_image;
    std::vector<char> after_image;
    microtime_t commit_timestamp = 0;
    std::string error_code;
    std::string redacted_diagnostic;
    microtime_t dead_lettered_at = 0;
};

// ── Sink driver interface (spec §6.5) ──────────────────────────────────────

// Abstract external delivery backend. One driver instance is bound to one
// sink configuration and lives on a single home thread. Implementations in
// this phase are stubs that model connect/deliver/close, ACK mapping, retry
// classification, and DLQ durability without third-party network libraries.
class cdc_sink_driver_t : public home_thread_mixin_t {
public:
    virtual ~cdc_sink_driver_t() = default;

    // Establish (or re-establish) the external connection using `config`.
    // Must validate non-secret connection parameters. Throws interrupted_exc_t
    // if `interruptor` is pulsed.
    virtual void connect(const cdc_sink_config_t &config,
                         signal_t *interruptor) = 0;

    // Deliver one contiguous single-shard batch. Returns OK with the highest
    // contiguous LSN only after a durable external write (or durable DLQ).
    // RETRYABLE_ERROR / PERMANENT_ERROR never advance the slot.
    virtual sink_delivery_result_t deliver(
        const publication_config_t &publication,
        const uuid_u &slot_id,
        const std::vector<change_record_t> &batch,
        signal_t *interruptor) = 0;

    // Tear down external resources. Idempotent.
    virtual void close(signal_t *interruptor) = 0;

    // Optional DLQ configuration. Empty connection_string means no DLQ.
    virtual void set_dead_letter_config(const dead_letter_config_t &dlq) = 0;

    // Last durable DLQ write observed by this driver (for tests / status).
    virtual const std::vector<dead_letter_record_t> &dead_letter_log() const = 0;

protected:
    cdc_sink_driver_t() = default;
};

// ── Concrete drivers (stubs — no external I/O libraries) ───────────────────

// Kafka: TLS/SASL param parsing, event IDs in key/header, batch flush,
// broker-outage modelled as retryable (spec §2.5, §5.6).
class kafka_sink_driver_t : public cdc_sink_driver_t {
public:
    explicit kafka_sink_driver_t(const cdc_sink_config_t &config);
    ~kafka_sink_driver_t() override;

    void connect(const cdc_sink_config_t &config,
                 signal_t *interruptor) override;
    sink_delivery_result_t deliver(
        const publication_config_t &publication,
        const uuid_u &slot_id,
        const std::vector<change_record_t> &batch,
        signal_t *interruptor) override;
    void close(signal_t *interruptor) override;
    void set_dead_letter_config(const dead_letter_config_t &dlq) override;
    const std::vector<dead_letter_record_t> &dead_letter_log() const override;

    // Parsed connection knobs (stub surface for tests / coordinator).
    bool tls_enabled() const { return tls_enabled_; }
    bool sasl_enabled() const { return sasl_enabled_; }
    const std::string &brokers() const { return brokers_; }
    const std::string &topic() const { return topic_; }
    bool connected() const { return connected_; }

    // Test hook: next deliver() returns retryable broker-outage error.
    void inject_broker_outage(bool enabled) { inject_outage_ = enabled; }

private:
    cdc_sink_config_t config_;
    dead_letter_config_t dlq_;
    std::vector<dead_letter_record_t> dlq_log_;
    std::string brokers_;
    std::string topic_;
    bool tls_enabled_ = false;
    bool sasl_enabled_ = false;
    bool connected_ = false;
    bool inject_outage_ = false;
    DISABLE_COPYING(kafka_sink_driver_t);
};

// Webhook: HTTPS POST, Idempotency-Key header, 2xx ACK, retry classification
// (429/5xx retryable, other 4xx permanent) — spec §2.5, §5.6.
class webhook_sink_driver_t : public cdc_sink_driver_t {
public:
    explicit webhook_sink_driver_t(const cdc_sink_config_t &config);
    ~webhook_sink_driver_t() override;

    void connect(const cdc_sink_config_t &config,
                 signal_t *interruptor) override;
    sink_delivery_result_t deliver(
        const publication_config_t &publication,
        const uuid_u &slot_id,
        const std::vector<change_record_t> &batch,
        signal_t *interruptor) override;
    void close(signal_t *interruptor) override;
    void set_dead_letter_config(const dead_letter_config_t &dlq) override;
    const std::vector<dead_letter_record_t> &dead_letter_log() const override;

    const std::string &url() const { return url_; }
    bool connected() const { return connected_; }

    // Test hooks for HTTP status classification without real I/O.
    void inject_http_status(int status) { inject_status_ = status; }
    void clear_injected_http_status() { inject_status_ = 0; }

    // Classify an HTTP status into a delivery outcome (exposed for unit tests).
    static sink_delivery_outcome_t classify_http_status(int status);

private:
    cdc_sink_config_t config_;
    dead_letter_config_t dlq_;
    std::vector<dead_letter_record_t> dlq_log_;
    std::string url_;
    bool connected_ = false;
    int inject_status_ = 0;  // 0 = success path (200)
    DISABLE_COPYING(webhook_sink_driver_t);
};

// File: write batch to path; object finalization + manifest is the ACK
// boundary (spec §2.5, §5.6).
class file_sink_driver_t : public cdc_sink_driver_t {
public:
    explicit file_sink_driver_t(const cdc_sink_config_t &config);
    ~file_sink_driver_t() override;

    void connect(const cdc_sink_config_t &config,
                 signal_t *interruptor) override;
    sink_delivery_result_t deliver(
        const publication_config_t &publication,
        const uuid_u &slot_id,
        const std::vector<change_record_t> &batch,
        signal_t *interruptor) override;
    void close(signal_t *interruptor) override;
    void set_dead_letter_config(const dead_letter_config_t &dlq) override;
    const std::vector<dead_letter_record_t> &dead_letter_log() const override;

    const std::string &path() const { return path_; }
    bool connected() const { return connected_; }
    // Number of finalized batch objects (manifest commits).
    uint64_t finalized_objects() const { return finalized_objects_; }
    // Last committed manifest summary (event range).
    const std::string &last_manifest() const { return last_manifest_; }

    // Test hook: fail before manifest finalization (no ACK).
    void inject_finalize_failure(bool enabled) { inject_finalize_fail_ = enabled; }

private:
    cdc_sink_config_t config_;
    dead_letter_config_t dlq_;
    std::vector<dead_letter_record_t> dlq_log_;
    std::string path_;
    bool connected_ = false;
    uint64_t finalized_objects_ = 0;
    std::string last_manifest_;
    bool inject_finalize_fail_ = false;
    DISABLE_COPYING(file_sink_driver_t);
};

// S3: same finalization+manifest ACK boundary as file, with bucket/prefix
// path construction (spec §2.5, §5.6).
class s3_sink_driver_t : public cdc_sink_driver_t {
public:
    explicit s3_sink_driver_t(const cdc_sink_config_t &config);
    ~s3_sink_driver_t() override;

    void connect(const cdc_sink_config_t &config,
                 signal_t *interruptor) override;
    sink_delivery_result_t deliver(
        const publication_config_t &publication,
        const uuid_u &slot_id,
        const std::vector<change_record_t> &batch,
        signal_t *interruptor) override;
    void close(signal_t *interruptor) override;
    void set_dead_letter_config(const dead_letter_config_t &dlq) override;
    const std::vector<dead_letter_record_t> &dead_letter_log() const override;

    const std::string &bucket() const { return bucket_; }
    const std::string &prefix() const { return prefix_; }
    bool connected() const { return connected_; }
    uint64_t finalized_objects() const { return finalized_objects_; }
    const std::string &last_manifest() const { return last_manifest_; }

    void inject_finalize_failure(bool enabled) { inject_finalize_fail_ = enabled; }

private:
    cdc_sink_config_t config_;
    dead_letter_config_t dlq_;
    std::vector<dead_letter_record_t> dlq_log_;
    std::string bucket_;
    std::string prefix_;
    bool connected_ = false;
    uint64_t finalized_objects_ = 0;
    std::string last_manifest_;
    bool inject_finalize_fail_ = false;
    DISABLE_COPYING(s3_sink_driver_t);
};

// ── Shared helpers ─────────────────────────────────────────────────────────

// Construct a driver for `config.sink_type`. Caller owns the pointer.
std::unique_ptr<cdc_sink_driver_t> make_cdc_sink_driver(
    const cdc_sink_config_t &config);

// Validate batching bounds (spec §5.5). Returns false and fills `error_out`
// when a field is out of range.
bool validate_batching_config(const cdc_batching_config_t &cfg,
                              std::string *error_out);

// Build a dead-letter record from a failed change_record (spec §8.4).
dead_letter_record_t make_dead_letter_record(
    const cdc_sink_config_t &sink,
    const uuid_u &slot_id,
    const uuid_u &publication_id,
    const change_record_t &record,
    const std::string &error_code,
    const std::string &redacted_diagnostic);

// Append `record` to an in-memory durable DLQ log. Returns true only when the
// write is considered durable (always true for the stub log). Slot advancement
// must gate on this return value (spec §8.4).
bool write_dead_letter(std::vector<dead_letter_record_t> *log,
                       const dead_letter_record_t &record,
                       signal_t *interruptor);

// Highest LSN in a non-empty single-shard batch. Guarantees all records share
// the same shard_id. Used by drivers when mapping durable write → ACK.
log_sequence_number_t batch_max_lsn(const std::vector<change_record_t> &batch);
uuid_u batch_shard_id(const std::vector<change_record_t> &batch);

// Format a stable event-id string for Kafka keys/headers and webhook
// Idempotency-Key values: cluster:table:shard:lsn.
std::string format_event_id(const change_event_id_t &id);

// Parse Kafka connection_string flags. Recognizes "tls=true"/"ssl=true" and
// "sasl=true"/"sasl_mechanism=..." tokens (comma or semicolon separated after
// the broker list). Brokers are the remaining host:port tokens.
void parse_kafka_connection_params(const std::string &connection_string,
                                   std::string *brokers_out,
                                   bool *tls_out,
                                   bool *sasl_out);

// Classify whether a permanent sink error should attempt DLQ (true) vs stay
// in ERROR without advancing (false when DLQ is unconfigured).
bool should_dead_letter(const dead_letter_config_t &dlq,
                        sink_delivery_outcome_t outcome);

}  // namespace ql

ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    ql::cdc_sink_type_t, int8_t,
    ql::cdc_sink_type_t::KAFKA, ql::cdc_sink_type_t::S3);
ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    ql::cdc_sink_state_t, int8_t,
    ql::cdc_sink_state_t::CREATING, ql::cdc_sink_state_t::ERROR);
ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    ql::sink_delivery_outcome_t, int8_t,
    ql::sink_delivery_outcome_t::OK, ql::sink_delivery_outcome_t::PERMANENT_ERROR);
RDB_DECLARE_SERIALIZABLE(ql::cdc_sink_config_t);
RDB_DECLARE_SERIALIZABLE(ql::cdc_batching_config_t);
RDB_DECLARE_SERIALIZABLE(ql::dead_letter_config_t);
RDB_DECLARE_SERIALIZABLE(ql::dead_letter_record_t);
RDB_DECLARE_SERIALIZABLE(ql::sink_delivery_result_t);

#endif  // RDB_PROTOCOL_CDC_SINK_HPP_
