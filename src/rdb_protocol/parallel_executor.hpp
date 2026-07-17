// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_PARALLEL_EXECUTOR_HPP_
#define RDB_PROTOCOL_PARALLEL_EXECUTOR_HPP_

/* Parallel query execution data structures (Phase 3 / PAR-01).

Fragment, plan, limits, executor, and merger interfaces. This header is the
data-structures phase only — no planning or execution logic lives here yet.

See .coding-hermes/specs/phase3-parallel-query.md §3.2–§3.7. */

#include <cstdint>
#include <string>
#include <vector>

#include "btree/keys.hpp"
#include "concurrency/cond_var.hpp"
#include "concurrency/signal.hpp"
#include "containers/scoped.hpp"
#include "errors.hpp"
#include "rdb_protocol/datum.hpp"
#include "threading.hpp"

class store_t;
struct read_t;
struct read_response_t;

/* Spec §3.4 — cancellation reason passed to request_cancel. */
class interruption_t {
public:
    interruption_t();
    explicit interruption_t(std::string reason);
    const std::string &reason() const;

private:
    std::string reason_;
};

/* Spec §3.4/§3.6 — error latched by the coordinator / merger.
 * Lightweight carrier for this data-structures phase; later phases may map it
 * onto ql::exc_t / cannot_perform_query_exc_t. */
class query_exc_t {
public:
    query_exc_t();
    explicit query_exc_t(std::string message);
    const std::string &message() const;

private:
    std::string message_;
};

/* query_fragment_t (§3.2) — identifies an input domain, never a pre-opened
 * cursor. Fragment boundaries are half-open [start, end) wherever the B-tree
 * range representation permits it. */
class query_fragment_t {
public:
    enum class kind_t {
        PRIMARY_KEY_RANGE,
        SECONDARY_INDEX_RANGE,
        FILTERED_PRIMARY_RANGE,
        PARTIAL_AGGREGATION
    };

    query_fragment_t(
        size_t ordinal,
        kind_t kind,
        key_range_t input_range,
        int64_t estimated_rows,
        int64_t estimated_bytes);

    size_t ordinal() const;
    kind_t kind() const;
    const key_range_t &input_range() const;
    int64_t estimated_rows() const;
    int64_t estimated_bytes() const;

private:
    size_t ordinal_;
    kind_t kind_;
    key_range_t input_range_;
    int64_t estimated_rows_;
    int64_t estimated_bytes_;
};

/* parallel_plan_t (§3.3) — complete executable plan from the planner. */
class parallel_plan_t {
public:
    enum class ordering_t { UNORDERED, PRIMARY_KEY_ASCENDING, EXPLICIT_ORDER };
    enum class terminal_t { STREAM, COUNT, SUM, AVG, MIN, MAX, REDUCE };

    parallel_plan_t(
        std::vector<query_fragment_t> fragments,
        ordering_t ordering,
        terminal_t terminal,
        int64_t estimated_serial_us,
        int64_t estimated_parallel_us);

    const std::vector<query_fragment_t> &fragments() const;
    ordering_t ordering() const;
    terminal_t terminal() const;
    bool preserves_serial_semantics() const;

    int64_t estimated_serial_us() const;
    int64_t estimated_parallel_us() const;

private:
    std::vector<query_fragment_t> fragments_;
    ordering_t ordering_;
    terminal_t terminal_;
    int64_t estimated_serial_us_;
    int64_t estimated_parallel_us_;
};

/* Planner result: either a complete plan or a serial-fallback reason. */
struct parallel_planning_result_t {
    scoped_ptr_t<parallel_plan_t> plan;
    std::string serial_reason;
};

/* parallel_execution_limits_t (§3.7) */
struct parallel_execution_limits_t {
    size_t max_workers;
    int64_t aggregate_buffer_bytes;
    int64_t per_worker_buffer_bytes;
    int64_t aggregate_timeout_ms;
    int64_t per_worker_timeout_ms;

    parallel_execution_limits_t()
        : max_workers(1),
          aggregate_buffer_bytes(0),
          per_worker_buffer_bytes(0),
          aggregate_timeout_ms(0),
          per_worker_timeout_ms(0) { }
};

/* fragment_batch_t (§3.5) */
struct fragment_batch_t {
    size_t fragment_ordinal;
    uint64_t sequence_number;
    std::vector<ql::datum_t> rows;
    int64_t encoded_bytes;
    bool end_of_fragment;

    fragment_batch_t()
        : fragment_ordinal(0),
          sequence_number(0),
          encoded_bytes(0),
          end_of_fragment(false) { }
};

/* partial_aggregate_t (§3.5) */
struct partial_aggregate_t {
    size_t fragment_ordinal;
    ql::datum_t state;
    int64_t input_rows;

    partial_aggregate_t()
        : fragment_ordinal(0),
          input_rows(0) { }
};

/* fragment_result_t (§3.5) — tagged payload from a worker to the coordinator. */
class fragment_result_t {
public:
    enum class state_t { BATCH, PARTIAL, COMPLETE, ERROR, CANCELED };

    explicit fragment_result_t(state_t state);

    state_t state() const;

    /* Payload accessors — valid only for the matching state. */
    const fragment_batch_t &batch() const;
    const partial_aggregate_t &partial() const;
    size_t complete_ordinal() const;
    const query_exc_t &error() const;

    static fragment_result_t make_batch(fragment_batch_t batch);
    static fragment_result_t make_partial(partial_aggregate_t partial);
    static fragment_result_t make_complete(size_t ordinal);
    static fragment_result_t make_error(query_exc_t error);
    static fragment_result_t make_canceled();

private:
    state_t state_;
    fragment_batch_t batch_;
    partial_aggregate_t partial_;
    size_t complete_ordinal_;
    query_exc_t error_;
};

/* result_merger_t (§3.6) */
class result_merger_t : public home_thread_mixin_t {
public:
    result_merger_t(
        parallel_plan_t::ordering_t ordering,
        int64_t memory_limit_bytes,
        signal_t *interruptor);
    ~result_merger_t();

    void push_batch(fragment_batch_t batch);
    void push_partial(partial_aggregate_t partial);
    void mark_fragment_complete(size_t ordinal);
    void fail(const query_exc_t &error);
    void drain_into(read_response_t *out);

private:
    void drain_unordered(read_response_t *out);
    void drain_ordered(read_response_t *out);
    void merge_partials(read_response_t *out);

    parallel_plan_t::ordering_t ordering_;
    int64_t memory_limit_bytes_;
    signal_t *interruptor_;

    DISABLE_COPYING(result_merger_t);
};

/* parallel_executor_t (§3.4) — sole owner of worker lifecycle. */
class parallel_executor_t : public home_thread_mixin_t {
public:
    parallel_executor_t(
        store_t *store,
        const read_t &base_read,
        const parallel_plan_t &plan,
        const parallel_execution_limits_t &limits,
        signal_t *parent_interruptor);
    ~parallel_executor_t();

    DISABLE_COPYING(parallel_executor_t);

    void run(read_response_t *response_out);
    void request_cancel(const interruption_t &reason);

private:
    void launch_worker(const query_fragment_t &fragment);
    void run_fragment(const query_fragment_t &fragment);
    void handle_worker_result(fragment_result_t result);
    void fail_all(const query_exc_t &error);
    void await_workers();

    store_t *store_;
    const read_t &base_read_;
    const parallel_plan_t &plan_;
    parallel_execution_limits_t limits_;
    signal_t *parent_interruptor_;
    cond_t canceled_;
    result_merger_t merger_;
};

/* parallel_admission_token_t (§3.7) — RAII reservation for workers + bytes. */
class parallel_admission_token_t {
public:
    parallel_admission_token_t(size_t workers, int64_t bytes);
    ~parallel_admission_token_t();

    size_t workers() const;
    int64_t bytes() const;

    DISABLE_COPYING(parallel_admission_token_t);

private:
    size_t workers_;
    int64_t bytes_;
};

#endif  // RDB_PROTOCOL_PARALLEL_EXECUTOR_HPP_
