// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/cdc_sink.hpp"

#include <cctype>
#include <utility>

#include "concurrency/interruptor.hpp"
#include "containers/archive/archive.hpp"
#include "containers/archive/stl_types.hpp"
#include "containers/archive/versioned.hpp"
#include "errors.hpp"
#include "time.hpp"
#include "utils.hpp"

// ── Serialization ──────────────────────────────────────────────────────────

RDB_IMPL_SERIALIZABLE_10_SINCE_v2_4(
    ql::cdc_sink_config_t,
    sink_id, name, publication_id, sink_type, connection_string,
    credential_ref, topic, state, created_by_user_id, created_at);
RDB_IMPL_SERIALIZABLE_4_SINCE_v2_4(
    ql::cdc_batching_config_t,
    max_records, max_in_flight_batches, flush_interval_ms, max_buffer_bytes);
RDB_IMPL_SERIALIZABLE_4_SINCE_v2_4(
    ql::dead_letter_config_t,
    type, connection_string, topic_or_prefix, credential_ref);
RDB_IMPL_SERIALIZABLE_11_SINCE_v2_4(
    ql::dead_letter_record_t,
    sink_id, slot_id, publication_id, event_id, op,
    before_image, after_image, commit_timestamp,
    error_code, redacted_diagnostic, dead_lettered_at);
RDB_IMPL_SERIALIZABLE_5_SINCE_v2_4(
    ql::sink_delivery_result_t,
    outcome, shard_id, acknowledged_lsn, error_message, dead_lettered);

namespace ql {

namespace {

void throw_if_interrupted(signal_t *interruptor) {
    if (interruptor != nullptr && interruptor->is_pulsed()) {
        throw interrupted_exc_t();
    }
}

sink_delivery_result_t make_ok_result(const std::vector<change_record_t> &batch) {
    sink_delivery_result_t r;
    r.outcome = sink_delivery_outcome_t::OK;
    r.shard_id = batch_shard_id(batch);
    r.acknowledged_lsn = batch_max_lsn(batch);
    r.dead_lettered = false;
    return r;
}

sink_delivery_result_t make_error_result(sink_delivery_outcome_t outcome,
                                         const std::string &message) {
    sink_delivery_result_t r;
    r.outcome = outcome;
    r.shard_id = nil_uuid();
    r.acknowledged_lsn = log_sequence_number_t{0};
    r.error_message = message;
    r.dead_lettered = false;
    return r;
}

// Attempt DLQ for a permanent failure on the first record of the batch.
// Returns a result that advances the slot only when every record's DLQ write
// is durable (spec §8.4).
sink_delivery_result_t try_dead_letter_batch(
        const cdc_sink_config_t &sink,
        const dead_letter_config_t &dlq,
        std::vector<dead_letter_record_t> *dlq_log,
        const publication_config_t &publication,
        const uuid_u &slot_id,
        const std::vector<change_record_t> &batch,
        const std::string &error_code,
        const std::string &diagnostic,
        signal_t *interruptor) {
    if (!should_dead_letter(dlq, sink_delivery_outcome_t::PERMANENT_ERROR)) {
        return make_error_result(
            sink_delivery_outcome_t::PERMANENT_ERROR,
            diagnostic + " (no dead-letter configured; slot not advanced)");
    }
    if (batch.empty()) {
        return make_error_result(sink_delivery_outcome_t::PERMANENT_ERROR,
                                 diagnostic);
    }
    for (const change_record_t &rec : batch) {
        throw_if_interrupted(interruptor);
        dead_letter_record_t dl = make_dead_letter_record(
            sink, slot_id, publication.publication_id, rec,
            error_code, diagnostic);
        if (!write_dead_letter(dlq_log, dl, interruptor)) {
            // DLQ unavailable — retain original record, do not advance.
            return make_error_result(
                sink_delivery_outcome_t::RETRYABLE_ERROR,
                "dead-letter write not durable; retaining original record");
        }
    }
    sink_delivery_result_t r = make_ok_result(batch);
    r.dead_lettered = true;
    r.error_message = diagnostic;
    return r;
}

// Build a stub JSON-ish envelope listing event IDs (no real JSON library).
std::string build_batch_envelope(const publication_config_t &publication,
                                 const uuid_u &slot_id,
                                 const std::vector<change_record_t> &batch) {
    std::string out = "{";
    out += "\"publication_id\":\"" + uuid_to_str(publication.publication_id) + "\",";
    out += "\"slot_id\":\"" + uuid_to_str(slot_id) + "\",";
    out += "\"records\":[";
    for (size_t i = 0; i < batch.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        out += "{\"event_id\":\"" + format_event_id(batch[i].event_id) + "\"";
        out += ",\"op\":" + std::to_string(static_cast<int>(batch[i].op));
        out += "}";
    }
    out += "]}";
    return out;
}

std::string build_manifest(const std::string &object_path,
                           const std::vector<change_record_t> &batch) {
    guarantee(!batch.empty());
    const change_event_id_t &first = batch.front().event_id;
    const change_event_id_t &last = batch.back().event_id;
    return strprintf(
        "object=%s shard=%s first_lsn=%llu last_lsn=%llu count=%zu",
        object_path.c_str(),
        uuid_to_str(first.shard_id).c_str(),
        static_cast<uint64_t>(first.lsn.value),
        static_cast<uint64_t>(last.lsn.value),
        batch.size());
}

std::string trim_token(const std::string &s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

std::string to_lower_copy(std::string s) {
    for (char &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

}  // namespace

// ── Shared helpers ─────────────────────────────────────────────────────────

bool validate_batching_config(const cdc_batching_config_t &cfg,
                              std::string *error_out) {
    // Bounds from spec §5.5 (slightly widened defaults already on the struct).
    if (cfg.max_records < 1 || cfg.max_records > 10000) {
        if (error_out != nullptr) {
            *error_out = strprintf(
                "max_records out of range [1, 10000]: %llu",
                static_cast<uint64_t>(cfg.max_records));
        }
        return false;
    }
    if (cfg.max_in_flight_batches < 1 || cfg.max_in_flight_batches > 64) {
        if (error_out != nullptr) {
            *error_out = strprintf(
                "max_in_flight_batches out of range [1, 64]: %llu",
                static_cast<uint64_t>(cfg.max_in_flight_batches));
        }
        return false;
    }
    if (cfg.flush_interval_ms < 1 || cfg.flush_interval_ms > 60000) {
        if (error_out != nullptr) {
            *error_out = strprintf(
                "flush_interval_ms out of range [1, 60000]: %llu",
                static_cast<uint64_t>(cfg.flush_interval_ms));
        }
        return false;
    }
    const uint64_t min_buf = 1ULL * 1024 * 1024;
    const uint64_t max_buf = 1ULL * 1024 * 1024 * 1024;
    if (cfg.max_buffer_bytes < min_buf || cfg.max_buffer_bytes > max_buf) {
        if (error_out != nullptr) {
            *error_out = strprintf(
                "max_buffer_bytes out of range [1MiB, 1GiB]: %llu",
                static_cast<uint64_t>(cfg.max_buffer_bytes));
        }
        return false;
    }
    return true;
}

dead_letter_record_t make_dead_letter_record(
        const cdc_sink_config_t &sink,
        const uuid_u &slot_id,
        const uuid_u &publication_id,
        const change_record_t &record,
        const std::string &error_code,
        const std::string &redacted_diagnostic) {
    dead_letter_record_t dl;
    dl.sink_id = sink.sink_id;
    dl.slot_id = slot_id;
    dl.publication_id = publication_id;
    dl.event_id = record.event_id;
    dl.op = record.op;
    dl.before_image = record.before_image;
    dl.after_image = record.after_image;
    dl.commit_timestamp = record.commit_timestamp;
    dl.error_code = error_code;
    dl.redacted_diagnostic = redacted_diagnostic;
    dl.dead_lettered_at = current_microtime();
    return dl;
}

bool write_dead_letter(std::vector<dead_letter_record_t> *log,
                       const dead_letter_record_t &record,
                       signal_t *interruptor) {
    guarantee(log != nullptr);
    throw_if_interrupted(interruptor);
    // Stub durable write: append is the durability boundary for in-process
    // tests. A real DLQ backend would fsync / broker-ack here before return.
    log->push_back(record);
    return true;
}

log_sequence_number_t batch_max_lsn(const std::vector<change_record_t> &batch) {
    guarantee(!batch.empty());
    log_sequence_number_t max_lsn = batch.front().event_id.lsn;
    const uuid_u shard = batch.front().event_id.shard_id;
    for (const change_record_t &r : batch) {
        guarantee(r.event_id.shard_id == shard,
                  "CDC sink batch must be single-shard (spec §5.5)");
        if (max_lsn < r.event_id.lsn) {
            max_lsn = r.event_id.lsn;
        }
    }
    return max_lsn;
}

uuid_u batch_shard_id(const std::vector<change_record_t> &batch) {
    guarantee(!batch.empty());
    const uuid_u shard = batch.front().event_id.shard_id;
    for (const change_record_t &r : batch) {
        guarantee(r.event_id.shard_id == shard,
                  "CDC sink batch must be single-shard (spec §5.5)");
    }
    return shard;
}

std::string format_event_id(const change_event_id_t &id) {
    return strprintf("%s:%s:%s:%llu",
                     uuid_to_str(id.source_cluster_id).c_str(),
                     uuid_to_str(id.table_id).c_str(),
                     uuid_to_str(id.shard_id).c_str(),
                     static_cast<uint64_t>(id.lsn.value));
}

void parse_kafka_connection_params(const std::string &connection_string,
                                   std::string *brokers_out,
                                   bool *tls_out,
                                   bool *sasl_out) {
    guarantee(brokers_out != nullptr);
    guarantee(tls_out != nullptr);
    guarantee(sasl_out != nullptr);
    *tls_out = false;
    *sasl_out = false;
    std::string brokers;
    std::string remaining = connection_string;
    // Split on comma or semicolon.
    size_t pos = 0;
    while (pos < remaining.size()) {
        size_t next = remaining.find_first_of(",;", pos);
        std::string tok = trim_token(
            remaining.substr(pos, next == std::string::npos
                                      ? std::string::npos
                                      : next - pos));
        pos = (next == std::string::npos) ? remaining.size() : next + 1;
        if (tok.empty()) {
            continue;
        }
        std::string lower = to_lower_copy(tok);
        if (lower == "tls=true" || lower == "ssl=true" || lower == "tls=1"
            || lower == "ssl=1") {
            *tls_out = true;
            continue;
        }
        if (lower == "tls=false" || lower == "ssl=false") {
            *tls_out = false;
            continue;
        }
        if (lower == "sasl=true" || lower == "sasl=1"
            || lower.find("sasl_mechanism=") == 0
            || lower.find("sasl.mechanism=") == 0) {
            *sasl_out = true;
            continue;
        }
        if (lower == "sasl=false") {
            *sasl_out = false;
            continue;
        }
        if (!brokers.empty()) {
            brokers.push_back(',');
        }
        brokers += tok;
    }
    *brokers_out = std::move(brokers);
}

bool should_dead_letter(const dead_letter_config_t &dlq,
                        sink_delivery_outcome_t outcome) {
    if (outcome != sink_delivery_outcome_t::PERMANENT_ERROR) {
        return false;
    }
    return !dlq.connection_string.empty();
}

std::unique_ptr<cdc_sink_driver_t> make_cdc_sink_driver(
        const cdc_sink_config_t &config) {
    switch (config.sink_type) {
    case cdc_sink_type_t::KAFKA:
        return std::unique_ptr<cdc_sink_driver_t>(
            new kafka_sink_driver_t(config));
    case cdc_sink_type_t::WEBHOOK:
        return std::unique_ptr<cdc_sink_driver_t>(
            new webhook_sink_driver_t(config));
    case cdc_sink_type_t::FILE:
        return std::unique_ptr<cdc_sink_driver_t>(
            new file_sink_driver_t(config));
    case cdc_sink_type_t::S3:
        return std::unique_ptr<cdc_sink_driver_t>(
            new s3_sink_driver_t(config));
    default:
        unreachable();
    }
}

// ── kafka_sink_driver_t ────────────────────────────────────────────────────

kafka_sink_driver_t::kafka_sink_driver_t(const cdc_sink_config_t &config)
    : config_(config) {
    topic_ = config.topic.str();
    parse_kafka_connection_params(
        config.connection_string, &brokers_, &tls_enabled_, &sasl_enabled_);
}

kafka_sink_driver_t::~kafka_sink_driver_t() {
    // Best-effort close without interruptor.
    connected_ = false;
}

void kafka_sink_driver_t::connect(const cdc_sink_config_t &config,
                                  signal_t *interruptor) {
    assert_thread();
    throw_if_interrupted(interruptor);
    config_ = config;
    topic_ = config.topic.str();
    parse_kafka_connection_params(
        config.connection_string, &brokers_, &tls_enabled_, &sasl_enabled_);
    if (brokers_.empty()) {
        // Leave disconnected; deliver will surface permanent config error.
        connected_ = false;
        return;
    }
    if (topic_.empty()) {
        connected_ = false;
        return;
    }
    // Stub: no librdkafka. TLS/SASL flags are recorded for the coordinator.
    connected_ = true;
}

sink_delivery_result_t kafka_sink_driver_t::deliver(
        const publication_config_t &publication,
        const uuid_u &slot_id,
        const std::vector<change_record_t> &batch,
        signal_t *interruptor) {
    assert_thread();
    throw_if_interrupted(interruptor);

    if (!connected_) {
        return make_error_result(
            sink_delivery_outcome_t::PERMANENT_ERROR,
            "kafka sink not connected");
    }
    if (batch.empty()) {
        return make_error_result(
            sink_delivery_outcome_t::PERMANENT_ERROR,
            "kafka deliver requires a non-empty batch");
    }

    // Model broker outage as retryable; slot must not advance (spec §5.6, §9.4).
    if (inject_outage_) {
        return make_error_result(
            sink_delivery_outcome_t::RETRYABLE_ERROR,
            "kafka broker outage (injected)");
    }

    // Construct per-record key/header event IDs and a batch envelope.
    // Stub: durability is modelled as successful local construction of the
    // producer payload. A real driver would wait for broker acks
    // (acks=all) before returning OK.
    for (const change_record_t &rec : batch) {
        throw_if_interrupted(interruptor);
        const std::string event_key = format_event_id(rec.event_id);
        // Key and header both carry the immutable event ID (spec §5.6).
        (void)event_key;
    }
    const std::string envelope =
        build_batch_envelope(publication, slot_id, batch);
    (void)envelope;

    // Durable broker write simulated — ACK highest contiguous LSN.
    return make_ok_result(batch);
}

void kafka_sink_driver_t::close(signal_t *interruptor) {
    assert_thread();
    throw_if_interrupted(interruptor);
    connected_ = false;
}

void kafka_sink_driver_t::set_dead_letter_config(const dead_letter_config_t &dlq) {
    assert_thread();
    dlq_ = dlq;
}

const std::vector<dead_letter_record_t> &
kafka_sink_driver_t::dead_letter_log() const {
    assert_thread();
    return dlq_log_;
}

// ── webhook_sink_driver_t ──────────────────────────────────────────────────

webhook_sink_driver_t::webhook_sink_driver_t(const cdc_sink_config_t &config)
    : config_(config), url_(config.connection_string) { }

webhook_sink_driver_t::~webhook_sink_driver_t() {
    connected_ = false;
}

void webhook_sink_driver_t::connect(const cdc_sink_config_t &config,
                                    signal_t *interruptor) {
    assert_thread();
    throw_if_interrupted(interruptor);
    config_ = config;
    url_ = config.connection_string;
    // Spec §10.3: HTTPS required except isolated test loopback fixtures.
    // Accept http:// only for loopback hosts so unit tests can use plain URLs.
    const bool https = url_.compare(0, 8, "https://") == 0;
    const bool http = url_.compare(0, 7, "http://") == 0;
    if (!https && !http) {
        connected_ = false;
        return;
    }
    if (http) {
        const std::string host = url_.substr(7);
        const bool loopback =
            host.compare(0, 9, "localhost") == 0
            || host.compare(0, 10, "127.0.0.1") == 0
            || host.compare(0, 3, "[::") == 0;
        if (!loopback) {
            connected_ = false;
            return;
        }
    }
    connected_ = true;
}

sink_delivery_outcome_t webhook_sink_driver_t::classify_http_status(int status) {
    if (status >= 200 && status < 300) {
        return sink_delivery_outcome_t::OK;
    }
    // 429 and 5xx are retryable; other 4xx are permanent (spec §5.6, §9.4).
    if (status == 429 || (status >= 500 && status <= 599)) {
        return sink_delivery_outcome_t::RETRYABLE_ERROR;
    }
    if (status >= 400 && status <= 499) {
        return sink_delivery_outcome_t::PERMANENT_ERROR;
    }
    // Timeouts / unknown transport failures surface as retryable.
    return sink_delivery_outcome_t::RETRYABLE_ERROR;
}

sink_delivery_result_t webhook_sink_driver_t::deliver(
        const publication_config_t &publication,
        const uuid_u &slot_id,
        const std::vector<change_record_t> &batch,
        signal_t *interruptor) {
    assert_thread();
    throw_if_interrupted(interruptor);

    if (!connected_) {
        return make_error_result(
            sink_delivery_outcome_t::PERMANENT_ERROR,
            "webhook sink not connected (invalid URL)");
    }
    if (batch.empty()) {
        return make_error_result(
            sink_delivery_outcome_t::PERMANENT_ERROR,
            "webhook deliver requires a non-empty batch");
    }

    // Stub HTTPS POST: body is the batch envelope; each record would carry
    // Idempotency-Key: <event_id> on a real client (spec §5.6).
    const std::string body =
        build_batch_envelope(publication, slot_id, batch);
    (void)body;
    for (const change_record_t &rec : batch) {
        throw_if_interrupted(interruptor);
        const std::string idempotency_key = format_event_id(rec.event_id);
        (void)idempotency_key;
    }

    const int status = (inject_status_ == 0) ? 200 : inject_status_;
    const sink_delivery_outcome_t outcome = classify_http_status(status);

    if (outcome == sink_delivery_outcome_t::OK) {
        // 2xx = receiver acceptance ACK boundary (at-least-once).
        return make_ok_result(batch);
    }

    const std::string diag = strprintf("webhook HTTP %d", status);
    if (outcome == sink_delivery_outcome_t::PERMANENT_ERROR) {
        return try_dead_letter_batch(
            config_, dlq_, &dlq_log_, publication, slot_id, batch,
            strprintf("http_%d", status), diag, interruptor);
    }
    return make_error_result(outcome, diag);
}

void webhook_sink_driver_t::close(signal_t *interruptor) {
    assert_thread();
    throw_if_interrupted(interruptor);
    connected_ = false;
}

void webhook_sink_driver_t::set_dead_letter_config(
        const dead_letter_config_t &dlq) {
    assert_thread();
    dlq_ = dlq;
}

const std::vector<dead_letter_record_t> &
webhook_sink_driver_t::dead_letter_log() const {
    assert_thread();
    return dlq_log_;
}

// ── file_sink_driver_t ─────────────────────────────────────────────────────

file_sink_driver_t::file_sink_driver_t(const cdc_sink_config_t &config)
    : config_(config), path_(config.connection_string) { }

file_sink_driver_t::~file_sink_driver_t() {
    connected_ = false;
}

void file_sink_driver_t::connect(const cdc_sink_config_t &config,
                                 signal_t *interruptor) {
    assert_thread();
    throw_if_interrupted(interruptor);
    config_ = config;
    path_ = config.connection_string;
    connected_ = !path_.empty();
}

sink_delivery_result_t file_sink_driver_t::deliver(
        const publication_config_t &publication,
        const uuid_u &slot_id,
        const std::vector<change_record_t> &batch,
        signal_t *interruptor) {
    assert_thread();
    throw_if_interrupted(interruptor);

    if (!connected_) {
        return make_error_result(
            sink_delivery_outcome_t::PERMANENT_ERROR,
            "file sink not connected (empty path)");
    }
    if (batch.empty()) {
        return make_error_result(
            sink_delivery_outcome_t::PERMANENT_ERROR,
            "file deliver requires a non-empty batch");
    }

    // Stage → finalize object → commit manifest. ACK only after manifest
    // commit (spec §5.6 File/S3). Stub keeps the staged payload in memory.
    const std::string envelope =
        build_batch_envelope(publication, slot_id, batch);
    const uuid_u shard = batch_shard_id(batch);
    const log_sequence_number_t max_lsn = batch_max_lsn(batch);
    const std::string object_path = strprintf(
        "%s/batch-%s-%llu.cdc",
        path_.c_str(),
        uuid_to_str(shard).c_str(),
        static_cast<uint64_t>(max_lsn.value));
    (void)envelope;

    if (inject_finalize_fail_) {
        // Interruption before finalization: no slot advancement (spec §9.4).
        return make_error_result(
            sink_delivery_outcome_t::RETRYABLE_ERROR,
            "file sink finalize interrupted before manifest commit");
    }

    // Manifest commit is the durable ACK boundary.
    last_manifest_ = build_manifest(object_path, batch);
    ++finalized_objects_;
    return make_ok_result(batch);
}

void file_sink_driver_t::close(signal_t *interruptor) {
    assert_thread();
    throw_if_interrupted(interruptor);
    connected_ = false;
}

void file_sink_driver_t::set_dead_letter_config(const dead_letter_config_t &dlq) {
    assert_thread();
    dlq_ = dlq;
}

const std::vector<dead_letter_record_t> &
file_sink_driver_t::dead_letter_log() const {
    assert_thread();
    return dlq_log_;
}

// ── s3_sink_driver_t ───────────────────────────────────────────────────────

s3_sink_driver_t::s3_sink_driver_t(const cdc_sink_config_t &config)
    : config_(config),
      bucket_(config.connection_string),
      prefix_(config.topic.str()) {
    if (prefix_.empty()) {
        prefix_ = "s3";
    }
}

s3_sink_driver_t::~s3_sink_driver_t() {
    connected_ = false;
}

void s3_sink_driver_t::connect(const cdc_sink_config_t &config,
                               signal_t *interruptor) {
    assert_thread();
    throw_if_interrupted(interruptor);
    config_ = config;
    bucket_ = config.connection_string;
    prefix_ = config.topic.str();
    if (prefix_.empty()) {
        prefix_ = "s3";
    }
    connected_ = !bucket_.empty();
}

sink_delivery_result_t s3_sink_driver_t::deliver(
        const publication_config_t &publication,
        const uuid_u &slot_id,
        const std::vector<change_record_t> &batch,
        signal_t *interruptor) {
    assert_thread();
    throw_if_interrupted(interruptor);

    if (!connected_) {
        return make_error_result(
            sink_delivery_outcome_t::PERMANENT_ERROR,
            "s3 sink not connected (empty bucket)");
    }
    if (batch.empty()) {
        return make_error_result(
            sink_delivery_outcome_t::PERMANENT_ERROR,
            "s3 deliver requires a non-empty batch");
    }

    const std::string envelope =
        build_batch_envelope(publication, slot_id, batch);
    const uuid_u shard = batch_shard_id(batch);
    const log_sequence_number_t max_lsn = batch_max_lsn(batch);
    // s3://bucket/prefix/batch-shard-lsn.cdc — final object path.
    std::string normalized_prefix = prefix_;
    if (!normalized_prefix.empty() && normalized_prefix.back() != '/') {
        normalized_prefix.push_back('/');
    }
    const std::string object_path = strprintf(
        "s3://%s/%sbatch-%s-%llu.cdc",
        bucket_.c_str(),
        normalized_prefix.c_str(),
        uuid_to_str(shard).c_str(),
        static_cast<uint64_t>(max_lsn.value));
    (void)envelope;

    if (inject_finalize_fail_) {
        return make_error_result(
            sink_delivery_outcome_t::RETRYABLE_ERROR,
            "s3 sink finalize interrupted before manifest commit");
    }

    last_manifest_ = build_manifest(object_path, batch);
    ++finalized_objects_;
    return make_ok_result(batch);
}

void s3_sink_driver_t::close(signal_t *interruptor) {
    assert_thread();
    throw_if_interrupted(interruptor);
    connected_ = false;
}

void s3_sink_driver_t::set_dead_letter_config(const dead_letter_config_t &dlq) {
    assert_thread();
    dlq_ = dlq;
}

const std::vector<dead_letter_record_t> &
s3_sink_driver_t::dead_letter_log() const {
    assert_thread();
    return dlq_log_;
}

}  // namespace ql
