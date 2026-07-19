// Copyright 2026 RethinkDB, all rights reserved.
#ifndef CLUSTERING_REPLICATION_MAILBOX_HPP_
#define CLUSTERING_REPLICATION_MAILBOX_HPP_

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "concurrency/auto_drainer.hpp"
#include "containers/archive/optional.hpp"
#include "containers/optional.hpp"
#include "containers/uuid.hpp"
#include "rdb_protocol/cdc_types.hpp"
#include "rdb_protocol/publication.hpp"
#include "rdb_protocol/subscription.hpp"
#include "rpc/mailbox/typed.hpp"
#include "rpc/serialize_macros.hpp"
#include "threading.hpp"
#include "time.hpp"

/* CDC-06d — Replication RPC mailbox service.
 *
 * Spec §5 (Frame & Protocol).
 *
 * Wire format: every mailbox payload exchanged between consumer (target) and
 * source (publisher) peers is a length-prefixed `replication_frame_t`. The
 * frame header carries the protocol version, the bound slot/stream UUID, the
 * frame type, and a bounded payload length. Receivers MUST validate the
 * payload length against a maximum frame size before allocating the payload
 * buffer (spec §5.3).
 *
 * Lifecycle (spec §5.2):
 *
 *   consumer -> source: HELLO                    (open + auth)
 *   consumer -> source: START_REPLICATION       (request snapshot mode + lsn)
 *   source    -> consumer: STARTED               (slot + barriers / live start)
 *   source    -> consumer: SNAPSHOT_BEGIN/ROW/END (optional, FULL snapshot)
 *   source    -> consumer: CHANGE_BATCH          (per-shard batch)
 *   consumer  -> source: ACK                     (per-shard confirmed lsn)
 *   either    -> either: HEARTBEAT / PAUSE / RESUME
 *   either    -> either: ERROR / CLOSE
 *   source    -> consumer: PUBLICATION_DROPPED / RESYNC_REQUIRED
 *
 * Both peers reject unsupported mandatory protocol versions before any data
 * frame is sent. This header defines the frame types, the framed envelope,
 * handshake structs, the business card (mailbox addresses), and a stub
 * `replication_mailbox_service_t` that owns the protocol state machine and
 * drainer for lifecycle management. The full wire loop will be wired up in
 * later CDC tasks; CDC-06d focuses on structural completeness so the
 * serialization surface compiles and is exposed for downstream code. */

namespace ql {

/* ── Frame type tags (spec §5.1) ─────────────────────────────────────────
 *
 * The numeric values are part of the wire protocol — DO NOT renumber
 * existing entries; append new tags at the end of the range. */

enum class replication_frame_type_t : uint8_t {
    HELLO              = 0,
    START_REPLICATION  = 1,
    STARTED            = 2,
    SNAPSHOT_BEGIN     = 3,
    SNAPSHOT_ROW       = 4,
    SNAPSHOT_END       = 5,
    CHANGE_BATCH       = 6,
    ACK                = 7,
    HEARTBEAT          = 8,
    PAUSE              = 9,
    RESUME             = 10,
    ERROR              = 11,
    PUBLICATION_DROPPED = 12,
    RESYNC_REQUIRED    = 13,
    CLOSE              = 14
};

/* Maximum payload length we'll accept. Mirrors the spec's recommended
 * upper bound on a single frame; receivers must enforce this BEFORE
 * allocating a payload vector. */
constexpr uint32_t REPLICATION_MAX_FRAME_PAYLOAD_BYTES = 16U * 1024U * 1024U;

/* Maximum number of change records packed into one CHANGE_BATCH frame.
 * Frames are framed at the cluster mailbox layer, so this is an internal
 * batching cap — keeps `std::vector<char> payload` bounded. */
constexpr uint32_t REPLICATION_MAX_BATCH_RECORDS = 4096;

/* Current protocol version. Bumped only when the wire format changes in a
 * way that cannot be carried in optional fields. */
constexpr uint32_t REPLICATION_PROTOCOL_VERSION = 1;

/* ── Frame envelope (spec §5.3) ──────────────────────────────────────── */

/* Wire header for every frame. The header is fixed-size; the bounded
 * `payload_length` field governs how many bytes follow in `payload`. */
struct replication_frame_header_t {
    uint32_t protocol_version = REPLICATION_PROTOCOL_VERSION;
    uuid_u stream_id;                    // slot/stream UUID
    replication_frame_type_t frame_type = replication_frame_type_t::HEARTBEAT;
    uint8_t flags = 0;                   // bit 0 = compressed, bit 1 = end-of-batch
    uint32_t payload_length = 0;         // bytes that follow in `payload`

    bool operator==(const replication_frame_header_t &other) const {
        return protocol_version == other.protocol_version
            && stream_id == other.stream_id
            && frame_type == other.frame_type
            && flags == other.flags
            && payload_length == other.payload_length;
    }
    bool operator!=(const replication_frame_header_t &other) const {
        return !(*this == other);
    }
};

/* Top-level framed payload. `payload` is the bytes of the inner record
 * (decoded by the caller based on `header.frame_type`). */
struct replication_frame_t {
    replication_frame_header_t header;
    std::vector<char> payload;

    bool operator==(const replication_frame_t &other) const {
        return header == other.header && payload == other.payload;
    }
    bool operator!=(const replication_frame_t &other) const {
        return !(*this == other);
    }
};

/* ── Handshake payloads (spec §5.2) ──────────────────────────────────── */

/* Consumer -> source: identity + slot binding request. */
struct replication_handshake_request_t {
    uuid_u publication_id;
    uuid_u consumer_id;
    uint32_t supported_protocol_version = REPLICATION_PROTOCOL_VERSION;
    snapshot_mode_t requested_snapshot_mode = snapshot_mode_t::FULL;
    /* Last LSNs the consumer has durably confirmed per shard. Empty on
     * first attach. */
    std::map<uuid_u, log_sequence_number_t> confirmed_lsn_by_shard;
    /* Schemas the consumer understands — opaque bytes for now; will be
     * parsed by the source in CDC-07. */
    std::vector<std::string> supported_schemas;

    bool operator==(const replication_handshake_request_t &other) const {
        return publication_id == other.publication_id
            && consumer_id == other.consumer_id
            && supported_protocol_version == other.supported_protocol_version
            && requested_snapshot_mode == other.requested_snapshot_mode
            && confirmed_lsn_by_shard == other.confirmed_lsn_by_shard
            && supported_schemas == other.supported_schemas;
    }
    bool operator!=(const replication_handshake_request_t &other) const {
        return !(*this == other);
    }
};

/* Source -> consumer: bound slot + initial positions. */
struct replication_handshake_response_t {
    uint32_t protocol_version = REPLICATION_PROTOCOL_VERSION;
    uuid_u source_cluster_id;
    uuid_u table_id;
    uuid_u publication_id;
    /* Slot UUID is nil until the source has bound or created a slot for
     * this consumer. */
    optional<uuid_u> slot_id;
    /* Per-shard barriers (FULL snapshot mode) or live-start LSNs. */
    std::vector<snapshot_barrier_t> barriers;
    /* Source cluster UUIDs are echoed back so the consumer can verify the
     * identity it expects. */
    bool operator==(const replication_handshake_response_t &other) const {
        return protocol_version == other.protocol_version
            && source_cluster_id == other.source_cluster_id
            && table_id == other.table_id
            && publication_id == other.publication_id
            && slot_id == other.slot_id
            && barriers == other.barriers;
    }
    bool operator!=(const replication_handshake_response_t &other) const {
        return !(*this == other);
    }
};

/* ── Per-frame record payloads ───────────────────────────────────────── */

/* CHANGE_BATCH payload: per-shard batch of change records. */
struct replication_change_batch_t {
    uuid_u shard_id;
    std::vector<ql::change_record_t> records;
};

/* ACK payload: per-shard confirmed LSN. */
struct replication_ack_t {
    uuid_u shard_id;
    log_sequence_number_t confirmed_lsn;

    bool operator==(const replication_ack_t &other) const {
        return shard_id == other.shard_id
            && confirmed_lsn == other.confirmed_lsn;
    }
    bool operator!=(const replication_ack_t &other) const {
        return !(*this == other);
    }
};

/* ERROR payload — diagnostic only, never carries credentials or row bytes. */
struct replication_error_payload_t {
    std::string code;
    std::string message;
    microtime_t occurred_at = 0;

    bool operator==(const replication_error_payload_t &other) const {
        return code == other.code
            && message == other.message
            && occurred_at == other.occurred_at;
    }
    bool operator!=(const replication_error_payload_t &other) const {
        return !(*this == other);
    }
};

/* ── Business card + service (spec §5.1) ────────────────────────────── */

/* Mailbox addresses the source exposes. A consumer reaches the source by
 * holding a `replication_mailbox_business_card_t`. */
class replication_mailbox_business_card_t {
public:
    /* RPC mailbox for handshake + control frames (HELLO, START, ACK, …). */
    typedef mailbox_t<replication_frame_t> frame_mailbox_t;
    typename frame_mailbox_t::address_t frame_mailbox;

    /* Reverse channel: source -> consumer for snapshot/change frames
     * that don't fit into request/reply correlation. */
    typedef mailbox_t<replication_frame_t> push_mailbox_t;
    typename push_mailbox_t::address_t push_mailbox;

    replication_mailbox_business_card_t() = default;
    replication_mailbox_business_card_t(
        const typename frame_mailbox_t::address_t &fm,
        const typename push_mailbox_t::address_t &pm)
        : frame_mailbox(fm), push_mailbox(pm) { }

    bool operator==(const replication_mailbox_business_card_t &other) const {
        return frame_mailbox == other.frame_mailbox
            && push_mailbox == other.push_mailbox;
    }
    bool operator!=(const replication_mailbox_business_card_t &other) const {
        return !(*this == other);
    }

    RDB_MAKE_ME_SERIALIZABLE_2(replication_mailbox_business_card_t,
        frame_mailbox, push_mailbox);
};

/* Service that owns the mailbox endpoints and drainer for a replication
 * session. CDC-06d ships the structural skeleton — handshake wiring,
 * TLS gating, and the framed read/write coroutines are stubbed here and
 * will be implemented in CDC-07/08 against the connectivity layer.
 *
 * Lifecycle: construct → call `get_business_card()` → publish the card
 * to peers via the directory → service accepts frames until destroyed. */
class replication_mailbox_service_t : public home_thread_mixin_t {
public:
    replication_mailbox_service_t(
        mailbox_manager_t *mailbox_manager,
        const uuid_u &stream_id,
        uint32_t protocol_version = REPLICATION_PROTOCOL_VERSION);

    ~replication_mailbox_service_t();

    /* Returns the business card (mailbox addresses) peers need to send
     * to this service. Must be called on the home thread. */
    replication_mailbox_business_card_t get_business_card() {
        assert_thread();
        replication_mailbox_business_card_t card;
        card.frame_mailbox = frame_mailbox.get_address();
        card.push_mailbox = push_mailbox.get_address();
        return card;
    }

    /* Bound stream/slot UUID. */
    const uuid_u &get_stream_id() const { return stream_id_; }

    /* Negotiated protocol version. May be lower than the requested
     * version after version handshake. */
    uint32_t negotiated_protocol_version() const {
        return negotiated_protocol_version_;
    }

    /* True if a remote peer has completed the handshake. */
    bool is_connected() const { return connected_; }

private:
    /* Frame dispatcher. Decodes `frame_type` and dispatches to typed
     * handlers. Stub for CDC-06d. */
    void on_frame(signal_t *interruptor, const replication_frame_t &frame);

    /* Push channel entry point (used for snapshot/change streams that
     * the consumer doesn't expect to ack in line). */
    void on_push(signal_t *interruptor, const replication_frame_t &frame);

    mailbox_manager_t *mailbox_manager_;

    /* Bound slot/stream id. */
    uuid_u stream_id_;

    /* Requested + negotiated protocol versions. */
    uint32_t requested_protocol_version_;
    uint32_t negotiated_protocol_version_;

    /* Set true after the handshake completes successfully. */
    bool connected_;

    /* Mailboxes that back the business card. */
    mailbox_t<replication_frame_t> frame_mailbox;
    mailbox_t<replication_frame_t> push_mailbox;

    auto_drainer_t drainer_;

    DISABLE_COPYING(replication_mailbox_service_t);
};

}  // namespace ql

ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    ql::replication_frame_type_t, uint8_t,
    ql::replication_frame_type_t::HELLO, ql::replication_frame_type_t::CLOSE);

RDB_DECLARE_SERIALIZABLE(ql::replication_frame_header_t);
RDB_DECLARE_SERIALIZABLE(ql::replication_frame_t);
RDB_DECLARE_SERIALIZABLE(ql::replication_handshake_request_t);
RDB_DECLARE_SERIALIZABLE(ql::replication_handshake_response_t);
RDB_DECLARE_SERIALIZABLE(ql::replication_change_batch_t);
RDB_DECLARE_SERIALIZABLE(ql::replication_ack_t);
RDB_DECLARE_SERIALIZABLE(ql::replication_error_payload_t);
RDB_DECLARE_SERIALIZABLE(ql::replication_mailbox_business_card_t);

#endif  // CLUSTERING_REPLICATION_MAILBOX_HPP_