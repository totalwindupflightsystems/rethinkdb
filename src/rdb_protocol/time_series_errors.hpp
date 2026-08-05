// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_TIME_SERIES_ERRORS_HPP_
#define RDB_PROTOCOL_TIME_SERIES_ERRORS_HPP_

/* Time-series error catalog — Phase 3 (spec §7).
 *
 * Each error code maps to a stable string identifier embedded in the
 * diagnostic message as a "[CODE] " prefix (same convention as the partition
 * error catalog). Client code and admin tooling can detect these codes
 * without depending on message phrasing.
 *
 * Only the config-layer codes are raised today (FIELD_MISSING,
 * CONFIG_IMMUTABLE, RETENTION_EXCEEDED, DOWNSAMPLE_CONFLICT). The remaining
 * codes are reserved for the storage-engine sub-tasks. */

#include <cstdarg>
#include <cstdio>

#include "rdb_protocol/error.hpp"

/* ── error code string constants (stable identifiers) ─────────────────────── */

namespace time_series_error {

/* Validation rejected before any metadata/storage change (→ LOGIC). */
constexpr const char *field_missing           = "TIME_SERIES_FIELD_MISSING";
constexpr const char *field_invalid_type      = "TIME_SERIES_FIELD_INVALID_TYPE";
constexpr const char *config_invalid          = "TIME_SERIES_CONFIG_INVALID";
constexpr const char *config_immutable        = "TIME_SERIES_CONFIG_IMMUTABLE";
constexpr const char *retention_exceeded      = "TIME_SERIES_RETENTION_EXCEEDED";

/* Read-path validation (PHASE3-TS-3): a between bound on the time-series
 * implicit index is not a ReQL time / r.minval / r.maxval (→ OP_FAILED). */
constexpr const char *bound_invalid            = "TIME_SERIES_BOUND_INVALID";

/* Storage-engine failures (later phases). */
constexpr const char *chunk_corrupt           = "TIME_SERIES_CHUNK_CORRUPT";
constexpr const char *downsample_conflict     = "TIME_SERIES_DOWNSAMPLE_CONFLICT";
constexpr const char *out_of_order_window     = "TIME_SERIES_OUT_OF_ORDER_WINDOW";
constexpr const char *chunk_overflow          = "TIME_SERIES_CHUNK_OVERFLOW";

/* ── UPPER_CASE aliases ────────────────────────────────────────────────── */

constexpr const char *TIME_SERIES_FIELD_MISSING = field_missing;
constexpr const char *TIME_SERIES_FIELD_INVALID_TYPE = field_invalid_type;
constexpr const char *TIME_SERIES_CONFIG_INVALID = config_invalid;
constexpr const char *TIME_SERIES_CONFIG_IMMUTABLE = config_immutable;
constexpr const char *TIME_SERIES_RETENTION_EXCEEDED = retention_exceeded;
constexpr const char *TIME_SERIES_BOUND_INVALID = bound_invalid;
constexpr const char *TIME_SERIES_CHUNK_CORRUPT = chunk_corrupt;
constexpr const char *TIME_SERIES_DOWNSAMPLE_CONFLICT = downsample_conflict;
constexpr const char *TIME_SERIES_OUT_OF_ORDER_WINDOW = out_of_order_window;
constexpr const char *TIME_SERIES_CHUNK_OVERFLOW = chunk_overflow;

/* ── internal helpers ─────────────────────────────────────────────────────── */

/* Build a "[CODE] user-message" string from printf-style args. */
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

inline void NORETURN raise_logic(
        const char *code, const char *fmt, ...) ATTR_FORMAT(printf, 2, 3);

inline void raise_logic(const char *code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string msg = format_code_msg(code, fmt, ap);
    va_end(ap);
    rfail_datum(ql::base_exc_t::LOGIC, "%s", msg.c_str());
}

inline void NORETURN raise_op_failed(
        const char *code, const char *fmt, ...) ATTR_FORMAT(printf, 2, 3);

inline void raise_op_failed(const char *code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string msg = format_code_msg(code, fmt, ap);
    va_end(ap);
    rfail_datum(ql::base_exc_t::OP_FAILED, "%s", msg.c_str());
}

inline void NORETURN raise_internal(
        const char *code, const char *fmt, ...) ATTR_FORMAT(printf, 2, 3);

inline void raise_internal(const char *code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string msg = format_code_msg(code, fmt, ap);
    va_end(ap);
    rfail_datum(ql::base_exc_t::INTERNAL, "%s", msg.c_str());
}

}  // namespace time_series_error

#endif  // RDB_PROTOCOL_TIME_SERIES_ERRORS_HPP_
