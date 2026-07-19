// Copyright 2026 RethinkDB, all rights reserved.
#include "clustering/replication_mailbox.hpp"

#include <utility>

#include "containers/archive/archive.hpp"
#include "containers/archive/stl_types.hpp"
#include "containers/archive/versioned.hpp"
#include "errors.hpp"
#include "rpc/mailbox/mailbox.hpp"
#include "utils.hpp"

/* ── Serialization implementations ─────────────────────────────────────
 *
 * These mirror the struct definitions in `replication_mailbox.hpp`. We
 * use the `_SINCE_v2_4` variants because CDC (Change Data Capture) is a
 * new wire format introduced after 2.4; downstream code that needs
 * cross-version compatibility will explicit-instantiate against the
 * matching cluster version. */

RDB_IMPL_SERIALIZABLE_5_SINCE_v2_4(
    ql::replication_frame_header_t,
    protocol_version, stream_id, frame_type, flags, payload_length);

RDB_IMPL_SERIALIZABLE_2_SINCE_v2_4(
    ql::replication_frame_t,
    header, payload);

RDB_IMPL_SERIALIZABLE_6_SINCE_v2_4(
    ql::replication_handshake_request_t,
    publication_id, consumer_id, supported_protocol_version,
    requested_snapshot_mode, confirmed_lsn_by_shard, supported_schemas);

RDB_IMPL_SERIALIZABLE_6_SINCE_v2_4(
    ql::replication_handshake_response_t,
    protocol_version, source_cluster_id, table_id, publication_id,
    slot_id, barriers);

RDB_IMPL_SERIALIZABLE_2_SINCE_v2_4(
    ql::replication_change_batch_t,
    shard_id, records);

RDB_IMPL_SERIALIZABLE_2_SINCE_v2_4(
    ql::replication_ack_t,
    shard_id, confirmed_lsn);

RDB_IMPL_SERIALIZABLE_3_SINCE_v2_4(
    ql::replication_error_payload_t,
    code, message, occurred_at);

RDB_IMPL_SERIALIZABLE_2_SINCE_v2_4(
    ql::replication_mailbox_business_card_t,
    frame_mailbox, push_mailbox);

namespace ql {

/* ── replication_mailbox_service_t ──────────────────────────────────── */

replication_mailbox_service_t::replication_mailbox_service_t(
    mailbox_manager_t *mailbox_manager,
    const uuid_u &stream_id,
    uint32_t protocol_version)
    : home_thread_mixin_t(get_thread_id()),
      mailbox_manager_(mailbox_manager),
      stream_id_(stream_id),
      requested_protocol_version_(protocol_version),
      negotiated_protocol_version_(protocol_version),
      connected_(false),
      frame_mailbox(
        mailbox_manager,
        std::bind(&replication_mailbox_service_t::on_frame,
                  this, ph::_1, ph::_2)),
      push_mailbox(
        mailbox_manager,
        std::bind(&replication_mailbox_service_t::on_push,
                  this, ph::_1, ph::_2)) { }

replication_mailbox_service_t::~replication_mailbox_service_t() {
    /* Drain all in-flight coroutines before destructing the mailboxes.
     * The mailboxes' readers are bound to `drainer_` indirectly via
     * the lifetime of `this`; pulsing the drainer (via its destructor
     * chain) ensures any callback coroutines have unwound. */
    auto_drainer_t::lock_t keepalive(&drainer_);
    /* Suppress unused-warning if no coroutine has ever taken a lock. */
    static_cast<void>(keepalive);
}

void replication_mailbox_service_t::on_frame(
    UNUSED signal_t *interruptor,
    UNUSED const replication_frame_t &frame) {
    /* Stub for CDC-06d. CDC-07 wires up the dispatch table keyed on
     * `frame.header.frame_type` and runs the protocol state machine. */
    assert_thread();
}

void replication_mailbox_service_t::on_push(
    UNUSED signal_t *interruptor,
    UNUSED const replication_frame_t &frame) {
    /* Stub for CDC-06d. Push channel is used for snapshot/change batches
     * that don't have a synchronous reply address. */
    assert_thread();
}

}  // namespace ql