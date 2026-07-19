// Copyright 2026 RethinkDB, all rights reserved.
#include "clustering/replication_mailbox.hpp"

#include <utility>

#include "arch/runtime/runtime.hpp"
#include "containers/archive/archive.hpp"
#include "containers/archive/optional.hpp"
#include "containers/archive/stl_types.hpp"
#include "containers/archive/versioned.hpp"
#include "errors.hpp"
#include "rpc/mailbox/mailbox.hpp"
#include "utils.hpp"

/* ── Serialization implementations ───────────────────────────────────────
 *
 * _SINCE_v2_4 variants — CDC is a new wire format introduced after v2.4.
 * The business card uses RDB_MAKE_ME_SERIALIZABLE_2 inline in the header,
 * so it is NOT repeated here. */

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

namespace ql {

/* ── replication_mailbox_service_t ───────────────────────────────────── */

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
      frame_mailbox(mailbox_manager,
          std::bind(&replication_mailbox_service_t::on_frame,
                    this, ph::_1, ph::_2)),
      push_mailbox(mailbox_manager,
          std::bind(&replication_mailbox_service_t::on_push,
                    this, ph::_1, ph::_2)) { }

replication_mailbox_service_t::~replication_mailbox_service_t() {
    assert_thread();
    drainer_.begin_draining();
    frame_mailbox.begin_shutdown();
    push_mailbox.begin_shutdown();
    drainer_.drain();
}

/* ── Callback stubs ────────────────────────────────────────────────────
 *
 * These are invoked by the mailbox infrastructure when frames arrive.
 * CDC-06d provides the structural skeleton; full protocol logic will be
 * implemented in later CDC tasks. */

void replication_mailbox_service_t::on_frame(
        signal_t * /*interruptor*/,
        const replication_frame_t & /*frame*/) {
    /* Stub: dispatch on frame_type, validate protocol version, route to
     * typed handlers (handshake, ack, heartbeat, close, ...). */
}

void replication_mailbox_service_t::on_push(
        signal_t * /*interruptor*/,
        const replication_frame_t & /*frame*/) {
    /* Stub: handle pushed frames (snapshot rows, change batches) from
     * the source side. */
}

}  // namespace ql
