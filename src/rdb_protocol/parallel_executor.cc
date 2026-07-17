// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/parallel_executor.hpp"

/* Stubs only (PAR-01). Full coordinator / merger logic is later phases. */

// ── interruption_t ──────────────────────────────────────────────────────────

interruption_t::interruption_t()
    : reason_("canceled") { }

interruption_t::interruption_t(std::string reason)
    : reason_(std::move(reason)) { }

const std::string &interruption_t::reason() const {
    return reason_;
}

// ── query_exc_t ─────────────────────────────────────────────────────────────

query_exc_t::query_exc_t()
    : message_("query error") { }

query_exc_t::query_exc_t(std::string message)
    : message_(std::move(message)) { }

const std::string &query_exc_t::message() const {
    return message_;
}

// ── query_fragment_t ────────────────────────────────────────────────────────

query_fragment_t::query_fragment_t(
    size_t ordinal,
    kind_t kind,
    key_range_t input_range,
    int64_t estimated_rows,
    int64_t estimated_bytes)
    : ordinal_(ordinal),
      kind_(kind),
      input_range_(std::move(input_range)),
      estimated_rows_(estimated_rows),
      estimated_bytes_(estimated_bytes) { }

size_t query_fragment_t::ordinal() const {
    return ordinal_;
}

query_fragment_t::kind_t query_fragment_t::kind() const {
    return kind_;
}

const key_range_t &query_fragment_t::input_range() const {
    return input_range_;
}

int64_t query_fragment_t::estimated_rows() const {
    return estimated_rows_;
}

int64_t query_fragment_t::estimated_bytes() const {
    return estimated_bytes_;
}

// ── parallel_plan_t ─────────────────────────────────────────────────────────

parallel_plan_t::parallel_plan_t(
    std::vector<query_fragment_t> fragments,
    ordering_t ordering,
    terminal_t terminal,
    int64_t estimated_serial_us,
    int64_t estimated_parallel_us)
    : fragments_(std::move(fragments)),
      ordering_(ordering),
      terminal_(terminal),
      estimated_serial_us_(estimated_serial_us),
      estimated_parallel_us_(estimated_parallel_us) { }

const std::vector<query_fragment_t> &parallel_plan_t::fragments() const {
    return fragments_;
}

parallel_plan_t::ordering_t parallel_plan_t::ordering() const {
    return ordering_;
}

parallel_plan_t::terminal_t parallel_plan_t::terminal() const {
    return terminal_;
}

bool parallel_plan_t::preserves_serial_semantics() const {
    /* Stub: full semantic check lands with the planner/executor. */
    return true;
}

int64_t parallel_plan_t::estimated_serial_us() const {
    return estimated_serial_us_;
}

int64_t parallel_plan_t::estimated_parallel_us() const {
    return estimated_parallel_us_;
}

// ── fragment_result_t ───────────────────────────────────────────────────────

fragment_result_t::fragment_result_t(state_t state)
    : state_(state),
      complete_ordinal_(0) { }

fragment_result_t::state_t fragment_result_t::state() const {
    return state_;
}

const fragment_batch_t &fragment_result_t::batch() const {
    return batch_;
}

const partial_aggregate_t &fragment_result_t::partial() const {
    return partial_;
}

size_t fragment_result_t::complete_ordinal() const {
    return complete_ordinal_;
}

const query_exc_t &fragment_result_t::error() const {
    return error_;
}

fragment_result_t fragment_result_t::make_batch(fragment_batch_t batch) {
    fragment_result_t r(state_t::BATCH);
    r.batch_ = std::move(batch);
    return r;
}

fragment_result_t fragment_result_t::make_partial(partial_aggregate_t partial) {
    fragment_result_t r(state_t::PARTIAL);
    r.partial_ = std::move(partial);
    return r;
}

fragment_result_t fragment_result_t::make_complete(size_t ordinal) {
    fragment_result_t r(state_t::COMPLETE);
    r.complete_ordinal_ = ordinal;
    return r;
}

fragment_result_t fragment_result_t::make_error(query_exc_t error) {
    fragment_result_t r(state_t::ERROR);
    r.error_ = std::move(error);
    return r;
}

fragment_result_t fragment_result_t::make_canceled() {
    return fragment_result_t(state_t::CANCELED);
}

// ── result_merger_t ─────────────────────────────────────────────────────────

result_merger_t::result_merger_t(
    parallel_plan_t::ordering_t ordering,
    int64_t memory_limit_bytes,
    signal_t *interruptor)
    : ordering_(ordering),
      memory_limit_bytes_(memory_limit_bytes),
      interruptor_(interruptor) { }

result_merger_t::~result_merger_t() { }

void result_merger_t::push_batch(fragment_batch_t) {
    /* stub */
}

void result_merger_t::push_partial(partial_aggregate_t) {
    /* stub */
}

void result_merger_t::mark_fragment_complete(size_t) {
    /* stub */
}

void result_merger_t::fail(const query_exc_t &) {
    /* stub */
}

void result_merger_t::drain_into(read_response_t *) {
    /* stub */
}

void result_merger_t::drain_unordered(read_response_t *) {
    /* stub */
}

void result_merger_t::drain_ordered(read_response_t *) {
    /* stub */
}

void result_merger_t::merge_partials(read_response_t *) {
    /* stub */
}

// ── parallel_executor_t ─────────────────────────────────────────────────────

parallel_executor_t::parallel_executor_t(
    store_t *store,
    const read_t &base_read,
    const parallel_plan_t &plan,
    const parallel_execution_limits_t &limits,
    signal_t *parent_interruptor)
    : store_(store),
      base_read_(base_read),
      plan_(plan),
      limits_(limits),
      parent_interruptor_(parent_interruptor),
      merger_(plan.ordering(),
              limits.aggregate_buffer_bytes,
              parent_interruptor) { }

parallel_executor_t::~parallel_executor_t() { }

void parallel_executor_t::run(read_response_t *) {
    /* stub */
}

void parallel_executor_t::request_cancel(const interruption_t &) {
    /* stub */
}

void parallel_executor_t::launch_worker(const query_fragment_t &) {
    /* stub */
}

void parallel_executor_t::run_fragment(const query_fragment_t &) {
    /* stub */
}

void parallel_executor_t::handle_worker_result(fragment_result_t) {
    /* stub */
}

void parallel_executor_t::fail_all(const query_exc_t &) {
    /* stub */
}

void parallel_executor_t::await_workers() {
    /* stub */
}

// ── parallel_admission_token_t ──────────────────────────────────────────────

parallel_admission_token_t::parallel_admission_token_t(
    size_t workers, int64_t bytes)
    : workers_(workers),
      bytes_(bytes) { }

parallel_admission_token_t::~parallel_admission_token_t() {
    /* stub — release reserved workers/bytes in a later phase */
}

size_t parallel_admission_token_t::workers() const {
    return workers_;
}

int64_t parallel_admission_token_t::bytes() const {
    return bytes_;
}
