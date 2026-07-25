// Copyright 2026 RethinkDB, all rights reserved.
#include "unittest/gtest.hpp"

#include <memory>
#include <string>
#include <vector>

#include "concurrency/cond_var.hpp"
#include "containers/uuid.hpp"
#include "rdb_protocol/cdc_sink.hpp"
#include "rdb_protocol/cdc_types.hpp"
#include "rdb_protocol/publication.hpp"

namespace unittest {

namespace {

ql::cdc_sink_config_t make_base_config(ql::cdc_sink_type_t type,
                                       const std::string &connection,
                                       const char *topic_name) {
    ql::cdc_sink_config_t cfg;
    cfg.sink_id = generate_uuid();
    cfg.name = name_string_t::guarantee_valid("test-sink");
    cfg.publication_id = generate_uuid();
    cfg.sink_type = type;
    cfg.connection_string = connection;
    cfg.topic = name_string_t::guarantee_valid(topic_name);
    cfg.state = ql::cdc_sink_state_t::CREATING;
    cfg.created_by_user_id = generate_uuid();
    cfg.created_at = 0;
    return cfg;
}

ql::publication_config_t make_publication() {
    ql::publication_config_t pub;
    pub.publication_id = generate_uuid();
    pub.name = name_string_t::guarantee_valid("events-pub");
    pub.database_id = generate_uuid();
    pub.table_id = generate_uuid();
    return pub;
}

ql::change_record_t make_record(const uuid_u &cluster,
                                const uuid_u &table,
                                const uuid_u &shard,
                                uint64_t lsn) {
    ql::change_record_t r;
    r.event_id = ql::change_event_id_t{cluster, table, shard, {lsn}};
    r.op = ql::change_operation_t::INSERT;
    r.after_image = {'x'};
    r.commit_timestamp = static_cast<microtime_t>(lsn * 1000);
    return r;
}

std::vector<ql::change_record_t> make_batch(const uuid_u &shard,
                                            uint64_t start_lsn,
                                            size_t count) {
    uuid_u cluster = generate_uuid();
    uuid_u table = generate_uuid();
    std::vector<ql::change_record_t> batch;
    batch.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        batch.push_back(make_record(cluster, table, shard, start_lsn + i));
    }
    return batch;
}

}  // namespace

// --- Batching validation ---

TEST(CdcSinkTest, ValidateBatchingConfigBounds) {
    ql::cdc_batching_config_t cfg;
    std::string err;
    EXPECT_TRUE(ql::validate_batching_config(cfg, &err));

    cfg.max_records = 0;
    EXPECT_FALSE(ql::validate_batching_config(cfg, &err));
    cfg.max_records = 1000;

    cfg.max_in_flight_batches = 100;
    EXPECT_FALSE(ql::validate_batching_config(cfg, &err));
    cfg.max_in_flight_batches = 5;

    cfg.flush_interval_ms = 0;
    EXPECT_FALSE(ql::validate_batching_config(cfg, &err));
    cfg.flush_interval_ms = 250;

    cfg.max_buffer_bytes = 100;
    EXPECT_FALSE(ql::validate_batching_config(cfg, &err));
}

// --- Helpers ---

TEST(CdcSinkTest, FormatEventId) {
    uuid_u cluster = generate_uuid();
    uuid_u table = generate_uuid();
    uuid_u shard = generate_uuid();
    ql::change_event_id_t id{cluster, table, shard, {42}};
    std::string s = ql::format_event_id(id);
    EXPECT_NE(std::string::npos, s.find(uuid_to_str(cluster)));
    EXPECT_NE(std::string::npos, s.find(":42"));
}

TEST(CdcSinkTest, ParseKafkaConnectionParamsTlsSasl) {
    std::string brokers;
    bool tls = false;
    bool sasl = false;
    ql::parse_kafka_connection_params(
        "kafka-1:9093,kafka-2:9093,tls=true,sasl_mechanism=SCRAM-SHA-256",
        &brokers, &tls, &sasl);
    EXPECT_EQ(brokers, "kafka-1:9093,kafka-2:9093");
    EXPECT_TRUE(tls);
    EXPECT_TRUE(sasl);
}

TEST(CdcSinkTest, BatchMaxLsnSingleShard) {
    uuid_u shard = generate_uuid();
    auto batch = make_batch(shard, 10, 3);
    EXPECT_EQ(ql::batch_shard_id(batch), shard);
    EXPECT_EQ(ql::batch_max_lsn(batch).value, 12U);
}

// --- Kafka driver ---

TEST(CdcSinkTest, KafkaConnectParsesTlsSaslAndDelivers) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::KAFKA,
        "broker-a:9093,broker-b:9093,tls=true,sasl=true",
        "events-topic");
    ql::kafka_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);

    EXPECT_TRUE(driver.connected());
    EXPECT_TRUE(driver.tls_enabled());
    EXPECT_TRUE(driver.sasl_enabled());
    EXPECT_EQ(driver.brokers(), "broker-a:9093,broker-b:9093");
    EXPECT_EQ(driver.topic(), "events-topic");

    auto pub = make_publication();
    uuid_u slot = generate_uuid();
    uuid_u shard = generate_uuid();
    auto batch = make_batch(shard, 100, 2);

    ql::sink_delivery_result_t res =
        driver.deliver(pub, slot, batch, &never);
    EXPECT_EQ(res.outcome, ql::sink_delivery_outcome_t::OK);
    EXPECT_EQ(res.shard_id, shard);
    EXPECT_EQ(res.acknowledged_lsn.value, 101U);
    EXPECT_FALSE(res.dead_lettered);

    driver.close(&never);
    EXPECT_FALSE(driver.connected());
}

TEST(CdcSinkTest, KafkaBrokerOutageIsRetryableNoAck) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::KAFKA, "broker:9093,tls=true", "t");
    ql::kafka_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);
    driver.inject_broker_outage(true);

    auto pub = make_publication();
    auto batch = make_batch(generate_uuid(), 1, 1);
    ql::sink_delivery_result_t res =
        driver.deliver(pub, generate_uuid(), batch, &never);
    EXPECT_EQ(res.outcome, ql::sink_delivery_outcome_t::RETRYABLE_ERROR);
    EXPECT_EQ(res.acknowledged_lsn.value, 0U);
}

// --- Webhook driver ---

TEST(CdcSinkTest, WebhookHttpsConnectAndDeliver) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::WEBHOOK,
        "https://receiver.example/v1/events",
        "webhook");
    ql::webhook_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);
    EXPECT_TRUE(driver.connected());

    auto pub = make_publication();
    uuid_u shard = generate_uuid();
    auto batch = make_batch(shard, 5, 1);
    ql::sink_delivery_result_t res =
        driver.deliver(pub, generate_uuid(), batch, &never);
    EXPECT_EQ(res.outcome, ql::sink_delivery_outcome_t::OK);
    EXPECT_EQ(res.acknowledged_lsn.value, 5U);
}

TEST(CdcSinkTest, WebhookHttpStatusClassification) {
    EXPECT_EQ(ql::webhook_sink_driver_t::classify_http_status(200),
              ql::sink_delivery_outcome_t::OK);
    EXPECT_EQ(ql::webhook_sink_driver_t::classify_http_status(204),
              ql::sink_delivery_outcome_t::OK);
    EXPECT_EQ(ql::webhook_sink_driver_t::classify_http_status(429),
              ql::sink_delivery_outcome_t::RETRYABLE_ERROR);
    EXPECT_EQ(ql::webhook_sink_driver_t::classify_http_status(503),
              ql::sink_delivery_outcome_t::RETRYABLE_ERROR);
    EXPECT_EQ(ql::webhook_sink_driver_t::classify_http_status(400),
              ql::sink_delivery_outcome_t::PERMANENT_ERROR);
    EXPECT_EQ(ql::webhook_sink_driver_t::classify_http_status(404),
              ql::sink_delivery_outcome_t::PERMANENT_ERROR);
}

TEST(CdcSinkTest, WebhookRetryableDoesNotAdvanceOrDlq) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::WEBHOOK,
        "https://receiver.example/v1/events",
        "webhook");
    ql::webhook_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);
    driver.inject_http_status(503);

    ql::dead_letter_config_t dlq;
    dlq.type = ql::cdc_sink_type_t::FILE;
    dlq.connection_string = "/tmp/dlq";
    driver.set_dead_letter_config(dlq);

    auto batch = make_batch(generate_uuid(), 9, 1);
    ql::sink_delivery_result_t res =
        driver.deliver(make_publication(), generate_uuid(), batch, &never);
    EXPECT_EQ(res.outcome, ql::sink_delivery_outcome_t::RETRYABLE_ERROR);
    EXPECT_EQ(res.acknowledged_lsn.value, 0U);
    EXPECT_TRUE(driver.dead_letter_log().empty());
}

TEST(CdcSinkTest, WebhookPermanentErrorWithDlqAdvancesAfterDurableWrite) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::WEBHOOK,
        "https://receiver.example/v1/events",
        "webhook");
    ql::webhook_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);
    driver.inject_http_status(400);

    ql::dead_letter_config_t dlq;
    dlq.type = ql::cdc_sink_type_t::FILE;
    dlq.connection_string = "/var/cdc/dlq";
    driver.set_dead_letter_config(dlq);

    uuid_u shard = generate_uuid();
    auto batch = make_batch(shard, 50, 2);
    ql::sink_delivery_result_t res =
        driver.deliver(make_publication(), generate_uuid(), batch, &never);
    EXPECT_EQ(res.outcome, ql::sink_delivery_outcome_t::OK);
    EXPECT_TRUE(res.dead_lettered);
    EXPECT_EQ(res.shard_id, shard);
    EXPECT_EQ(res.acknowledged_lsn.value, 51U);
    EXPECT_EQ(driver.dead_letter_log().size(), 2U);
    EXPECT_EQ(driver.dead_letter_log()[0].event_id.lsn.value, 50U);
}

TEST(CdcSinkTest, WebhookPermanentErrorWithoutDlqDoesNotAdvance) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::WEBHOOK,
        "https://receiver.example/v1/events",
        "webhook");
    ql::webhook_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);
    driver.inject_http_status(422);

    auto batch = make_batch(generate_uuid(), 1, 1);
    ql::sink_delivery_result_t res =
        driver.deliver(make_publication(), generate_uuid(), batch, &never);
    EXPECT_EQ(res.outcome, ql::sink_delivery_outcome_t::PERMANENT_ERROR);
    EXPECT_EQ(res.acknowledged_lsn.value, 0U);
    EXPECT_FALSE(res.dead_lettered);
}

// --- File / S3 drivers ---

TEST(CdcSinkTest, FileDeliverFinalizesManifestAsAck) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::FILE, "/var/cdc/archive", "file");
    ql::file_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);
    EXPECT_TRUE(driver.connected());

    uuid_u shard = generate_uuid();
    auto batch = make_batch(shard, 7, 3);
    ql::sink_delivery_result_t res =
        driver.deliver(make_publication(), generate_uuid(), batch, &never);
    EXPECT_EQ(res.outcome, ql::sink_delivery_outcome_t::OK);
    EXPECT_EQ(res.acknowledged_lsn.value, 9U);
    EXPECT_EQ(driver.finalized_objects(), 1U);
    EXPECT_NE(std::string::npos, driver.last_manifest().find("first_lsn=7"));
    EXPECT_NE(std::string::npos, driver.last_manifest().find("last_lsn=9"));
}

TEST(CdcSinkTest, FileFinalizeFailureDoesNotAck) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::FILE, "/var/cdc/archive", "file");
    ql::file_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);
    driver.inject_finalize_failure(true);

    auto batch = make_batch(generate_uuid(), 1, 1);
    ql::sink_delivery_result_t res =
        driver.deliver(make_publication(), generate_uuid(), batch, &never);
    EXPECT_EQ(res.outcome, ql::sink_delivery_outcome_t::RETRYABLE_ERROR);
    EXPECT_EQ(res.acknowledged_lsn.value, 0U);
    EXPECT_EQ(driver.finalized_objects(), 0U);
}

TEST(CdcSinkTest, S3DeliverUsesBucketPrefixAndManifestAck) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::S3, "analytics-archive", "rethinkdb-events");
    ql::s3_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);
    EXPECT_TRUE(driver.connected());
    EXPECT_EQ(driver.bucket(), "analytics-archive");
    EXPECT_EQ(driver.prefix(), "rethinkdb-events");

    uuid_u shard = generate_uuid();
    auto batch = make_batch(shard, 20, 1);
    ql::sink_delivery_result_t res =
        driver.deliver(make_publication(), generate_uuid(), batch, &never);
    EXPECT_EQ(res.outcome, ql::sink_delivery_outcome_t::OK);
    EXPECT_EQ(res.acknowledged_lsn.value, 20U);
    EXPECT_EQ(driver.finalized_objects(), 1U);
    EXPECT_NE(std::string::npos,
              driver.last_manifest().find("s3://analytics-archive/"));
}

// --- Factory + dead-letter helper ---

TEST(CdcSinkTest, MakeCdcSinkDriverFactory) {
    cond_t never;
    {
        auto cfg = make_base_config(
            ql::cdc_sink_type_t::KAFKA, "b:9093", "t");
        auto d = ql::make_cdc_sink_driver(cfg);
        ASSERT_NE(d.get(), nullptr);
        d->connect(cfg, &never);
    }
    {
        auto cfg = make_base_config(
            ql::cdc_sink_type_t::WEBHOOK, "https://example.com/hook", "webhook");
        auto d = ql::make_cdc_sink_driver(cfg);
        ASSERT_NE(d.get(), nullptr);
        d->connect(cfg, &never);
    }
    {
        auto cfg = make_base_config(
            ql::cdc_sink_type_t::FILE, "/tmp/out", "file");
        auto d = ql::make_cdc_sink_driver(cfg);
        ASSERT_NE(d.get(), nullptr);
    }
    {
        auto cfg = make_base_config(
            ql::cdc_sink_type_t::S3, "bucket", "prefix");
        auto d = ql::make_cdc_sink_driver(cfg);
        ASSERT_NE(d.get(), nullptr);
    }
}

TEST(CdcSinkTest, WriteDeadLetterIsDurableBoundary) {
    cond_t never;
    std::vector<ql::dead_letter_record_t> log;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::KAFKA, "b:9093", "t");
    auto rec = make_record(generate_uuid(), generate_uuid(),
                           generate_uuid(), 3);
    ql::dead_letter_record_t dl = ql::make_dead_letter_record(
        cfg, generate_uuid(), generate_uuid(), rec,
        "permanent_reject", "redacted");
    EXPECT_TRUE(ql::write_dead_letter(&log, dl, &never));
    ASSERT_EQ(log.size(), 1U);
    EXPECT_EQ(log[0].event_id.lsn.value, 3U);
    EXPECT_EQ(log[0].error_code, "permanent_reject");
}

TEST(CdcSinkTest, ShouldDeadLetterOnlyPermanentWithConfig) {
    ql::dead_letter_config_t empty;
    ql::dead_letter_config_t cfg;
    cfg.connection_string = "/dlq";
    EXPECT_FALSE(ql::should_dead_letter(
        empty, ql::sink_delivery_outcome_t::PERMANENT_ERROR));
    EXPECT_FALSE(ql::should_dead_letter(
        cfg, ql::sink_delivery_outcome_t::RETRYABLE_ERROR));
    EXPECT_TRUE(ql::should_dead_letter(
        cfg, ql::sink_delivery_outcome_t::PERMANENT_ERROR));
}

// ── CDC-10: edge case / boundary tests ──

// Kafka edge cases

TEST(CdcSinkTest, KafkaEmptyBrokerString) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::KAFKA, "", "events");
    ql::kafka_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);
    // Empty broker string → not connected
    EXPECT_FALSE(driver.connected());
    EXPECT_TRUE(driver.brokers().empty());
}

// name_string_t enforces non-empty at construction, so empty-topic
// edge case is prevented at the type level. Test reconnection instead.
TEST(CdcSinkTest, KafkaReconnectAfterClose) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::KAFKA, "broker:9093", "events");
    ql::kafka_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);
    EXPECT_TRUE(driver.connected());

    driver.close(&never);
    EXPECT_FALSE(driver.connected());

    // Reconnect should work
    driver.connect(cfg, &never);
    EXPECT_TRUE(driver.connected());
}

TEST(CdcSinkTest, KafkaNotConnectedDeliverFails) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::KAFKA, "", "t");
    ql::kafka_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);
    EXPECT_FALSE(driver.connected());
    auto batch = make_batch(generate_uuid(), 1, 1);
    ql::sink_delivery_result_t res =
        driver.deliver(make_publication(), generate_uuid(), batch, &never);
    EXPECT_EQ(res.outcome, ql::sink_delivery_outcome_t::PERMANENT_ERROR);
    EXPECT_EQ(res.acknowledged_lsn.value, 0U);
}

TEST(CdcSinkTest, KafkaEmptyBatchDeliverFails) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::KAFKA, "broker:9093", "t");
    ql::kafka_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);
    EXPECT_TRUE(driver.connected());
    std::vector<ql::change_record_t> empty_batch;
    ql::sink_delivery_result_t res =
        driver.deliver(make_publication(), generate_uuid(), empty_batch, &never);
    EXPECT_EQ(res.outcome, ql::sink_delivery_outcome_t::PERMANENT_ERROR);
}

// Webhook edge cases

TEST(CdcSinkTest, WebhookNonLoopbackHttpRejected) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::WEBHOOK,
        "http://public-server.example/hook",
        "wh");
    ql::webhook_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);
    // Non-loopback HTTP should be rejected (spec §10.3)
    EXPECT_FALSE(driver.connected());
}

TEST(CdcSinkTest, WebhookLoopbackHttpAccepted) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::WEBHOOK,
        "http://localhost:8080/hook",
        "wh");
    ql::webhook_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);
    EXPECT_TRUE(driver.connected());
}

TEST(CdcSinkTest, WebhookEmptyUrlRejected) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::WEBHOOK, "", "wh");
    ql::webhook_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);
    EXPECT_FALSE(driver.connected());
}

TEST(CdcSinkTest, WebhookEmptyBatchDeliverFails) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::WEBHOOK,
        "https://receiver.example/v1/events",
        "wh");
    ql::webhook_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);
    std::vector<ql::change_record_t> empty_batch;
    ql::sink_delivery_result_t res =
        driver.deliver(make_publication(), generate_uuid(), empty_batch, &never);
    EXPECT_EQ(res.outcome, ql::sink_delivery_outcome_t::PERMANENT_ERROR);
}

TEST(CdcSinkTest, WebhookHttpStatusBoundaryValues) {
    // Edge-case HTTP status codes
    EXPECT_EQ(ql::webhook_sink_driver_t::classify_http_status(199),
              ql::sink_delivery_outcome_t::RETRYABLE_ERROR);  // not 2xx
    EXPECT_EQ(ql::webhook_sink_driver_t::classify_http_status(200),
              ql::sink_delivery_outcome_t::OK);
    EXPECT_EQ(ql::webhook_sink_driver_t::classify_http_status(299),
              ql::sink_delivery_outcome_t::OK);
    EXPECT_EQ(ql::webhook_sink_driver_t::classify_http_status(300),
              ql::sink_delivery_outcome_t::RETRYABLE_ERROR);
    EXPECT_EQ(ql::webhook_sink_driver_t::classify_http_status(308),
              ql::sink_delivery_outcome_t::RETRYABLE_ERROR);  // 3xx redirect
    EXPECT_EQ(ql::webhook_sink_driver_t::classify_http_status(400),
              ql::sink_delivery_outcome_t::PERMANENT_ERROR);
    EXPECT_EQ(ql::webhook_sink_driver_t::classify_http_status(401),
              ql::sink_delivery_outcome_t::PERMANENT_ERROR);
    EXPECT_EQ(ql::webhook_sink_driver_t::classify_http_status(405),
              ql::sink_delivery_outcome_t::PERMANENT_ERROR);
    EXPECT_EQ(ql::webhook_sink_driver_t::classify_http_status(408),
              ql::sink_delivery_outcome_t::PERMANENT_ERROR);
    EXPECT_EQ(ql::webhook_sink_driver_t::classify_http_status(418),
              ql::sink_delivery_outcome_t::PERMANENT_ERROR);
    EXPECT_EQ(ql::webhook_sink_driver_t::classify_http_status(500),
              ql::sink_delivery_outcome_t::RETRYABLE_ERROR);
    EXPECT_EQ(ql::webhook_sink_driver_t::classify_http_status(502),
              ql::sink_delivery_outcome_t::RETRYABLE_ERROR);
    EXPECT_EQ(ql::webhook_sink_driver_t::classify_http_status(599),
              ql::sink_delivery_outcome_t::RETRYABLE_ERROR);
    EXPECT_EQ(ql::webhook_sink_driver_t::classify_http_status(600),
              ql::sink_delivery_outcome_t::RETRYABLE_ERROR);
}

// File edge cases

TEST(CdcSinkTest, FileEmptyPathNotConnected) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::FILE, "", "file");
    ql::file_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);
    EXPECT_FALSE(driver.connected());
}

TEST(CdcSinkTest, FileEmptyBatchDeliverFails) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::FILE, "/tmp/out", "file");
    ql::file_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);
    std::vector<ql::change_record_t> empty_batch;
    ql::sink_delivery_result_t res =
        driver.deliver(make_publication(), generate_uuid(), empty_batch, &never);
    EXPECT_EQ(res.outcome, ql::sink_delivery_outcome_t::PERMANENT_ERROR);
}

TEST(CdcSinkTest, FileMultipleDeliversAccumulateObjects) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::FILE, "/var/cdc/archive", "file");
    ql::file_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);

    uuid_u shard = generate_uuid();
    for (int i = 0; i < 5; ++i) {
        auto batch = make_batch(shard, static_cast<uint64_t>(i * 10 + 1),
                                static_cast<size_t>(3));
        ql::sink_delivery_result_t res =
            driver.deliver(make_publication(), generate_uuid(), batch, &never);
        EXPECT_EQ(res.outcome, ql::sink_delivery_outcome_t::OK);
    }
    EXPECT_EQ(driver.finalized_objects(), 5U);
}

// S3 edge cases

TEST(CdcSinkTest, S3EmptyBucketNotConnected) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::S3, "", "prefix");
    ql::s3_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);
    EXPECT_FALSE(driver.connected());
}

TEST(CdcSinkTest, S3EmptyBatchDeliverFails) {
    cond_t never;
    auto cfg = make_base_config(
        ql::cdc_sink_type_t::S3, "my-bucket", "prefix");
    ql::s3_sink_driver_t driver(cfg);
    driver.connect(cfg, &never);
    std::vector<ql::change_record_t> empty_batch;
    ql::sink_delivery_result_t res =
        driver.deliver(make_publication(), generate_uuid(), empty_batch, &never);
    EXPECT_EQ(res.outcome, ql::sink_delivery_outcome_t::PERMANENT_ERROR);
}

TEST(CdcSinkTest, S3PrefixNormalization) {
    cond_t never;
    // Without trailing slash
    auto cfg1 = make_base_config(
        ql::cdc_sink_type_t::S3, "bucket", "prefix-no-slash");
    ql::s3_sink_driver_t driver1(cfg1);
    driver1.connect(cfg1, &never);
    EXPECT_TRUE(driver1.connected());

    uuid_u shard = generate_uuid();
    auto batch = make_batch(shard, 1, 1);
    ql::sink_delivery_result_t res =
        driver1.deliver(make_publication(), generate_uuid(), batch, &never);
    EXPECT_EQ(res.outcome, ql::sink_delivery_outcome_t::OK);
    EXPECT_NE(std::string::npos,
              driver1.last_manifest().find("s3://bucket/prefix-no-slash/"));
}

// Batching boundary values

TEST(CdcSinkTest, ValidateBatchingBoundaryMinValues) {
    ql::cdc_batching_config_t cfg;
    std::string err;
    // Min valid values
    cfg.max_records = 1;
    cfg.max_in_flight_batches = 1;
    cfg.flush_interval_ms = 1;
    cfg.max_buffer_bytes = 1ULL * 1024 * 1024;
    EXPECT_TRUE(ql::validate_batching_config(cfg, &err));
}

TEST(CdcSinkTest, ValidateBatchingBoundaryMaxValues) {
    ql::cdc_batching_config_t cfg;
    std::string err;
    // Max valid values
    cfg.max_records = 10000;
    cfg.max_in_flight_batches = 64;
    cfg.flush_interval_ms = 60000;
    cfg.max_buffer_bytes = 1ULL * 1024 * 1024 * 1024;
    EXPECT_TRUE(ql::validate_batching_config(cfg, &err));
}

// Parse kafka: brokers-only (no TLS/SASL flags)

TEST(CdcSinkTest, ParseKafkaBrokersOnlyNoFlags) {
    std::string brokers;
    bool tls = false;
    bool sasl = false;
    ql::parse_kafka_connection_params(
        "host-a:9092,host-b:9092",
        &brokers, &tls, &sasl);
    EXPECT_EQ(brokers, "host-a:9092,host-b:9092");
    EXPECT_FALSE(tls);
    EXPECT_FALSE(sasl);
}

TEST(CdcSinkTest, ParseKafkaTlsExplicitFalse) {
    std::string brokers;
    bool tls = true;  // start true, should be set to false
    bool sasl = false;
    ql::parse_kafka_connection_params(
        "broker:9093,tls=false",
        &brokers, &tls, &sasl);
    EXPECT_EQ(brokers, "broker:9093");
    EXPECT_FALSE(tls);
    EXPECT_FALSE(sasl);
}

TEST(CdcSinkTest, ParseKafkaSemispaceNotComma) {
    std::string brokers;
    bool tls = false;
    bool sasl = false;
    ql::parse_kafka_connection_params(
        "k1:9093;tls=true;sasl=true",
        &brokers, &tls, &sasl);
    EXPECT_EQ(brokers, "k1:9093");
    EXPECT_TRUE(tls);
    EXPECT_TRUE(sasl);
}

// ── CDC-10: compatibility tests ──

// Backward compat: change_record_t with old-style empty fields
TEST(CdcSinkTest, ChangeRecordBackwardCompatEmptyFields) {
    // A zero-initialized change_record_t with all fields zeroed
    ql::change_record_t rec{};
    // Zero-initialized: op is INSERT (0), timestamp 0, empty images
    EXPECT_EQ(rec.op, ql::change_operation_t::INSERT);
    EXPECT_EQ(rec.commit_timestamp, 0u);
    EXPECT_TRUE(rec.before_image.empty());
    EXPECT_TRUE(rec.after_image.empty());
}

// Forward compat: new event_id fields with old code paths
TEST(CdcSinkTest, FormatEventIdHandlesAllZeroUuids) {
    ql::change_event_id_t id;
    // All-zero UUIDs and LSN should still format
    std::string s = ql::format_event_id(id);
    EXPECT_FALSE(s.empty());
    EXPECT_NE(std::string::npos, s.find(":0"));
}

// Mixed-version: sink types are stable
TEST(CdcSinkTest, SinkTypeEnumIsStable) {
    // Sink type enum values must not change for backward compat
    EXPECT_EQ(static_cast<int>(ql::cdc_sink_type_t::KAFKA), 0);
    EXPECT_EQ(static_cast<int>(ql::cdc_sink_type_t::WEBHOOK), 1);
    EXPECT_EQ(static_cast<int>(ql::cdc_sink_type_t::FILE), 2);
    EXPECT_EQ(static_cast<int>(ql::cdc_sink_type_t::S3), 3);
}

TEST(CdcSinkTest, SinkStateEnumIsStable) {
    EXPECT_EQ(static_cast<int>(ql::cdc_sink_state_t::CREATING), 0);
    EXPECT_EQ(static_cast<int>(ql::cdc_sink_state_t::CONNECTING), 1);
    EXPECT_EQ(static_cast<int>(ql::cdc_sink_state_t::STREAMING), 2);
    EXPECT_EQ(static_cast<int>(ql::cdc_sink_state_t::DROPPING), 3);
    EXPECT_EQ(static_cast<int>(ql::cdc_sink_state_t::DROPPED), 4);
    EXPECT_EQ(static_cast<int>(ql::cdc_sink_state_t::ERROR), 5);
}

}  // namespace unittest
