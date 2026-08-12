// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_CDC_PUMP_HPP_
#define RDB_PROTOCOL_CDC_PUMP_HPP_

#include <set>
#include <string>

#include "concurrency/auto_drainer.hpp"
#include "containers/scoped.hpp"
#include "rdb_protocol/cdc_types.hpp"
#include "rdb_protocol/changefeed.hpp"
#include "rdb_protocol/publication.hpp"
#include "rdb_protocol/subscription.hpp"

class namespace_repo_t;
class rdb_context_t;
class table_meta_client_t;

namespace ql {

/* RT-GAP-015 step 4: the CDC streaming pump.
 *
 * The pump is the target-side replication coordinator for CDC
 * subscriptions. It polls the cluster's table metadata for subscriptions
 * in CREATING state, drives each one through the lifecycle transitions
 * (CREATING → CONNECTING → CATCHING_UP → STREAMING), opens a changefeed
 * stream on the publication's source table, converts each change datum
 * ({old_val, new_val}) into a change_record_t, and applies the records to
 * the target table through the applier's target-writer seam.
 *
 * The pump runs on the home thread of the real_reql_cluster_interface_t
 * that owns it. It is spawned in the interface constructor and keeps
 * itself alive with an auto_drainer_t::lock_t; the drainer is destroyed
 * with the interface, which stops the pump before the underlying
 * namespace_repo_t / changefeed client are torn down.
 *
 * Scope of this step (honest limitations):
 *  - Snapshot mode is NONE: the pump drives subscriptions through
 *    CATCHING_UP with an empty live-start position set, so only changes
 *    that occur AFTER the pump opens the feed are delivered. FULL
 *    snapshot delivery (SNAPSHOTTING) is a later step.
 *  - The changefeed stream is opened with include_initial=false, so no
 *    pre-existing rows are replayed.
 *  - The pump is single-threaded and polls metadata on a fixed interval;
 *    it is a functional end-to-end path, not a high-throughput pipeline.
 */
class cdc_pump_t : public home_thread_mixin_t {
public:
    cdc_pump_t(rdb_context_t *rdb_context,
               table_meta_client_t *table_meta_client,
               namespace_repo_t *namespace_repo,
               ql::changefeed::client_t *changefeed_client);
    ~cdc_pump_t();

    /* Poll interval for metadata scans. */
    static const int64_t POLL_INTERVAL_MS = 1000;

private:
    void run(auto_drainer_t::lock_t keepalive);

    /* Scan all tables for subscriptions in CREATING state and spawn a
     * stream coroutine for each one not already in flight. */
    void drive_subscriptions(signal_t *interruptor,
                             auto_drainer_t::lock_t keepalive);

    /* Per-subscription coroutine: drive the lifecycle transitions and
     * stream changes from the source table to the target table until the
     * stream ends or the drain signal fires. */
    void stream_subscription_coro(subscription_config_t sub,
                                  publication_config_t pub,
                                  std::string table_name,
                                  auto_drainer_t::lock_t keepalive);

    /* Open a changefeed stream on the source table and apply every change
     * to the target table until the stream ends or the interruptor fires.
     * Returns the number of records applied. */
    size_t stream_subscription(subscription_handle_t *handle,
                               subscription_applier_t *applier,
                               const uuid_u &source_table_id,
                               const std::string &table_name,
                               signal_t *interruptor);

    /* Convert a changefeed change datum ({old_val, new_val}) into a
     * change_record_t. `lsn` is the per-shard incrementing sequence
     * number; `shard_id` is generated per stream. */
    change_record_t change_datum_to_record(
        const datum_t &change,
        const uuid_u &source_cluster_id,
        const uuid_u &source_table_id,
        const uuid_u &shard_id,
        log_sequence_number_t lsn);

    rdb_context_t *rdb_context_;
    table_meta_client_t *table_meta_client_;
    namespace_repo_t *namespace_repo_;
    ql::changefeed::client_t *changefeed_client_;

    /* Subscription ids currently being streamed. Only touched on the home
     * thread (the pump and its stream coroutines are cooperative). */
    std::set<uuid_u> in_flight_subscriptions_;

    auto_drainer_t drainer_;

    DISABLE_COPYING(cdc_pump_t);
};

}  // namespace ql

#endif  // RDB_PROTOCOL_CDC_PUMP_HPP_
