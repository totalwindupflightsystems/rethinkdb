// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_PARTITION_ERRORS_HPP_
#define RDB_PROTOCOL_PARTITION_ERRORS_HPP_

/* Partition error catalog — Phase 3 declarative partitioning (spec §7).

   Each error code maps to a stable string identifier embedded in the
   diagnostic message as a "[CODE] " prefix. Client code and status/admin
   tooling can detect these codes without depending on message phrasing.

   Three conditions from the spec table reuse existing error infrastructure:
     - Duplicate global PK → ordinary duplicate-PK error
     - Analyzer cannot prove a constraint → no error (safe fallback)
     - Child sindex not ready → existing sindex-not-ready error

   raise_<category>(code, fmt, ...) wraps rfail_datum with the appropriate
   base_exc_t::type_t per condition.                                           */

#include <cstdarg>
#include <cstdio>

#include "rdb_protocol/error.hpp"

/* ── error code string constants (stable identifiers) ─────────────────────── */

namespace partition_error_code {

/* Validation rejected before any metadata/storage change (→ LOGIC). */
constexpr const char *config_invalid         = "PARTITION_CONFIG_INVALID";
constexpr const char *range_invalid          = "PARTITION_RANGE_INVALID";
constexpr const char *hash_invalid           = "PARTITION_HASH_INVALID";
constexpr const char *list_invalid           = "PARTITION_LIST_INVALID";

/* Key extraction rejected before B-tree mutation (→ LOGIC). */
constexpr const char *key_missing            = "PARTITION_KEY_MISSING";
constexpr const char *key_invalid            = "PARTITION_KEY_INVALID";

/* Operational / quorum / storage failures. */
constexpr const char *key_unroutable        = "PARTITION_KEY_UNROUTABLE";
constexpr const char *move_failed           = "PARTITION_MOVE_FAILED";
constexpr const char *query_limit           = "PARTITION_QUERY_LIMIT";
constexpr const char *metadata_corrupt      = "PARTITION_METADATA_CORRUPT";
constexpr const char *transition_busy       = "PARTITION_TRANSITION_BUSY";
constexpr const char *backfill_overflow     = "PARTITION_BACKFILL_OVERFLOW";
constexpr const char *storage_unavailable   = "PARTITION_STORAGE_UNAVAILABLE";
constexpr const char *raft_timeout          = "PARTITION_RAFT_TIMEOUT";
constexpr const char *metadata_incompatible  = "PARTITION_METADATA_INCOMPATIBLE";

/* ── internal helpers ─────────────────────────────────────────────────────── */

/* Build a "[CODE] user-message" string from printf-style args.
   Returns the formatted buffer. Truncates to 1023 chars + NUL if overflow. */
inline std::string format_code_msg(const char *code, const char *fmt,
                                   va_list ap) {
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "[%s] ", code);
    if (n < 0) n = 0;
    size_t off = static_cast<size_t>(n);
    if (off >= sizeof(buf)) off = sizeof(buf) - 1;
    vsnprintf(buf + off, sizeof(buf) - off, fmt, ap);
    return std::string(buf);
}

/* ── raise helpers ───────────────────────────────────────────────────────── */

/* LOGIC: client-side, user-correctable (invalid config / key / field). */
inline void NORETURN raise_logic(
        const char *code, const char *fmt, ...) ATTR_FORMAT(printf, 2, 3);

inline void raise_logic(const char *code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string msg = format_code_msg(code, fmt, ap);
    va_end(ap);
    rfail_datum(ql::base_exc_t::LOGIC, "%s", msg.c_str());
}

/* OP_FAILED: operation known to have failed (storage, quorum, transition). */
inline void NORETURN raise_op_failed(
        const char *code, const char *fmt, ...) ATTR_FORMAT(printf, 2, 3);

inline void raise_op_failed(const char *code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string msg = format_code_msg(code, fmt, ap);
    va_end(ap);
    rfail_datum(ql::base_exc_t::OP_FAILED, "%s", msg.c_str());
}

/* OP_INDETERMINATE: uncertain whether operation succeeded or failed
   (Raft timeout, unconfirmed cutover). */
inline void NORETURN raise_op_indeterminate(
        const char *code, const char *fmt, ...) ATTR_FORMAT(printf, 2, 3);

inline void raise_op_indeterminate(const char *code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string msg = format_code_msg(code, fmt, ap);
    va_end(ap);
    rfail_datum(ql::base_exc_t::OP_INDETERMINATE, "%s", msg.c_str());
}

/* RESOURCE: exceeded a configured limit (query fan-out, partition count). */
inline void NORETURN raise_resource(
        const char *code, const char *fmt, ...) ATTR_FORMAT(printf, 2, 3);

inline void raise_resource(const char *code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string msg = format_code_msg(code, fmt, ap);
    va_end(ap);
    rfail_datum(ql::base_exc_t::RESOURCE, "%s", msg.c_str());
}

/* INTERNAL: corrupt metadata, unrecoverable catalog state. */
inline void NORETURN raise_internal(
        const char *code, const char *fmt, ...) ATTR_FORMAT(printf, 2, 3);

inline void raise_internal(const char *code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string msg = format_code_msg(code, fmt, ap);
    va_end(ap);
    rfail_datum(ql::base_exc_t::INTERNAL, "%s", msg.c_str());
}

}  // namespace partition_error_code

#endif  // RDB_PROTOCOL_PARTITION_ERRORS_HPP_
