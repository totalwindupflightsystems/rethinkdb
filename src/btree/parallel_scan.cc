// Copyright 2026 RethinkDB, all rights reserved.
#include "btree/parallel_scan.hpp"

/* Stubs only (PAR-01). Isolated B-tree range scans and quantile sampling land
 * in PAR-04. */

parallel_scan_request_t::parallel_scan_request_t()
    : fragment_ordinal(0),
      estimated_rows(0),
      estimated_bytes(0) { }

parallel_scan_request_t::parallel_scan_request_t(
    size_t ordinal,
    key_range_t range,
    int64_t estimated_rows,
    int64_t estimated_bytes)
    : fragment_ordinal(ordinal),
      range(std::move(range)),
      estimated_rows(estimated_rows),
      estimated_bytes(estimated_bytes) { }

parallel_scan_state_t::parallel_scan_state_t()
    : fragment_ordinal(0),
      exhausted(true),
      rows_scanned(0),
      bytes_scanned(0) { }

parallel_scan_state_t::parallel_scan_state_t(
    const parallel_scan_request_t &request)
    : fragment_ordinal(request.fragment_ordinal),
      range(request.range),
      exhausted(false),
      rows_scanned(0),
      bytes_scanned(0) { }

int64_t parallel_scan_t::estimate_row_count(
    superblock_t *,
    const key_range_t &,
    signal_t *) {
    /* stub */
    return 0;
}

std::vector<store_key_t> parallel_scan_t::sample_key_quantiles(
    superblock_t *,
    const key_range_t &,
    size_t,
    signal_t *) {
    /* stub */
    return std::vector<store_key_t>();
}

bool parallel_scan_t::validate_fragment_coverage(
    const key_range_t &,
    const std::vector<key_range_t> &) {
    /* stub */
    return true;
}

parallel_scan_split_t parallel_scan_t::split_range(
    const key_range_t &,
    const std::vector<store_key_t> &) {
    /* stub */
    return parallel_scan_split_t();
}

parallel_scan_state_t parallel_scan_t::init_scan_state(
    const parallel_scan_request_t &request) {
    return parallel_scan_state_t(request);
}
