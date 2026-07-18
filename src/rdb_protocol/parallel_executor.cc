// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/parallel_executor.hpp"

#include <climits>
#include <string>

#include "arch/runtime/coroutines.hpp"
#include "arch/timing.hpp"
#include "concurrency/interruptor.hpp"
#include "errors.hpp"
#include "protocol_api.hpp"
#include "rdb_protocol/error.hpp"
#include "rdb_protocol/protocol.hpp"
#include "rdb_protocol/shards.hpp"
#include "rdb_protocol/store.hpp"
#include "store_view.hpp"

// ── interruption_t ──────────────────────────────────────────────────────────

interruption_t::interruption_t() : reason_("canceled") { }
interruption_t::interruption_t(std::string reason) : reason_(std::move(reason)) { }
const std::string &interruption_t::reason() const { return reason_; }

// ── query_exc_t ─────────────────────────────────────────────────────────────

query_exc_t::query_exc_t() : message_("query error") { }
query_exc_t::query_exc_t(std::string message) : message_(std::move(message)) { }
const std::string &query_exc_t::message() const { return message_; }

// ── query_fragment_t ────────────────────────────────────────────────────────

query_fragment_t::query_fragment_t(size_t ordinal, kind_t kind,
    key_range_t input_range, int64_t estimated_rows, int64_t estimated_bytes)
    : ordinal_(ordinal), kind_(kind), input_range_(std::move(input_range)),
      estimated_rows_(estimated_rows), estimated_bytes_(estimated_bytes) { }
size_t query_fragment_t::ordinal() const { return ordinal_; }
auto query_fragment_t::kind() const -> kind_t { return kind_; }
const key_range_t &query_fragment_t::input_range() const { return input_range_; }
int64_t query_fragment_t::estimated_rows() const { return estimated_rows_; }
int64_t query_fragment_t::estimated_bytes() const { return estimated_bytes_; }

// ── parallel_plan_t ─────────────────────────────────────────────────────────

parallel_plan_t::parallel_plan_t(std::vector<query_fragment_t> fragments,
    ordering_t ordering, terminal_t terminal, int64_t estimated_serial_us,
    int64_t estimated_parallel_us)
    : fragments_(std::move(fragments)), ordering_(ordering), terminal_(terminal),
      estimated_serial_us_(estimated_serial_us),
      estimated_parallel_us_(estimated_parallel_us) { }
const std::vector<query_fragment_t> &parallel_plan_t::fragments() const {
    return fragments_;
}
auto parallel_plan_t::ordering() const -> ordering_t { return ordering_; }
auto parallel_plan_t::terminal() const -> terminal_t { return terminal_; }
bool parallel_plan_t::preserves_serial_semantics() const { return true; }
int64_t parallel_plan_t::estimated_serial_us() const {
    return estimated_serial_us_;
}
int64_t parallel_plan_t::estimated_parallel_us() const {
    return estimated_parallel_us_;
}

// ── fragment_result_t ───────────────────────────────────────────────────────

fragment_result_t::fragment_result_t(state_t state)
    : state_(state), complete_ordinal_(0) { }
auto fragment_result_t::state() const -> state_t { return state_; }
const fragment_batch_t &fragment_result_t::batch() const { return batch_; }
const partial_aggregate_t &fragment_result_t::partial() const { return partial_; }
size_t fragment_result_t::complete_ordinal() const { return complete_ordinal_; }
const query_exc_t &fragment_result_t::error() const { return error_; }

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

static int64_t batch_cost(const fragment_batch_t &b) {
    return b.encoded_bytes > 0
        ? b.encoded_bytes
        : static_cast<int64_t>(b.rows.size()) * 256;
}

result_merger_t::result_merger_t(parallel_plan_t::ordering_t ordering,
    parallel_plan_t::terminal_t terminal, size_t total_fragments,
    int64_t memory_limit_bytes, signal_t *interruptor)
    : ordering_(ordering),
      terminal_(terminal),
      total_fragments_(total_fragments),
      memory_limit_bytes_(memory_limit_bytes),
      memory_used_bytes_(0),
      interruptor_(interruptor),
      failed_(false),
      ordered_queues_(
          ordering == parallel_plan_t::ordering_t::UNORDERED
              ? 0
              : total_fragments),
      ordered_drain_ordinal_(0) { }

result_merger_t::~result_merger_t() { }

bool result_merger_t::all_fragments_complete() const {
    return completed_fragments_.size() == total_fragments_ || failed_;
}

bool result_merger_t::failed() const {
    return failed_;
}

int64_t result_merger_t::memory_used_bytes() const {
    return memory_used_bytes_;
}

size_t result_merger_t::completed_count() const {
    return completed_fragments_.size();
}

void result_merger_t::assert_memory_invariants() const {
#ifndef NDEBUG
    rassert(memory_used_bytes_ >= 0);
    if (memory_limit_bytes_ > 0) {
        rassert(memory_used_bytes_ <= memory_limit_bytes_);
    }
#endif
}

void result_merger_t::push_batch(fragment_batch_t batch) {
    /* Spec §7.6: no batch merged after terminal failure. */
    if (failed_) {
        return;
    }

#ifndef NDEBUG
    rassert(batch.fragment_ordinal < total_fragments_);
#endif

    int64_t cost = batch_cost(batch);
    while (memory_limit_bytes_ > 0
           && memory_used_bytes_ + cost > memory_limit_bytes_
           && !failed_
           && !interruptor_->is_pulsed()) {
        wait_any_t w(&space_available_, interruptor_);
        w.wait_lazily_unordered();
    }
    if (failed_ || interruptor_->is_pulsed()) {
        return;
    }

    memory_used_bytes_ += cost;
    assert_memory_invariants();

    if (ordering_ == parallel_plan_t::ordering_t::UNORDERED) {
        unordered_queue_.push_back(std::move(batch));
    } else {
        guarantee(batch.fragment_ordinal < ordered_queues_.size());
        ordered_queues_[batch.fragment_ordinal].push_back(std::move(batch));
    }
}

void result_merger_t::push_partial(partial_aggregate_t partial) {
    if (failed_) {
        return;
    }
#ifndef NDEBUG
    rassert(partial.fragment_ordinal < total_fragments_);
#endif
    partials_.push_back(std::move(partial));
}

void result_merger_t::mark_fragment_complete(size_t ordinal) {
#ifndef NDEBUG
    rassert(ordinal < total_fragments_ || total_fragments_ == 0);
    /* At most once under success; under failure duplicates are tolerated. */
    if (!failed_) {
        rassert(completed_fragments_.count(ordinal) == 0);
    }
#endif
    completed_fragments_.insert(ordinal);
    space_available_.pulse_if_not_already_pulsed();
}

void result_merger_t::fail(const query_exc_t &) {
    failed_ = true;
    unordered_queue_.clear();
    for (auto &q : ordered_queues_) {
        q.clear();
    }
    partials_.clear();
    memory_used_bytes_ = 0;
    space_available_.pulse_if_not_already_pulsed();
}

static void build_stream_response(
    read_response_t *out,
    std::vector<ql::datum_t> &&rows) {
    ql::stream_t stream;
    auto &sub = stream.substreams[region_t::universe()];
    for (auto &row : rows) {
        sub.stream.push_back(
            ql::rget_item_t(store_key_t(), ql::datum_t(), std::move(row)));
    }
    ql::grouped_t<ql::stream_t> grouped;
    grouped.insert(std::make_pair(ql::datum_t(), std::move(stream)));
    rget_read_response_t rget;
    rget.result = std::move(grouped);
    out->response = std::move(rget);
}

void result_merger_t::drain_into(read_response_t *out) {
    guarantee(out != nullptr);
    if (failed_) {
        return;
    }

#ifndef NDEBUG
    rassert(completed_fragments_.size() == total_fragments_);
#endif

    if (!partials_.empty()
        || terminal_ == parallel_plan_t::terminal_t::COUNT
        || terminal_ == parallel_plan_t::terminal_t::SUM
        || terminal_ == parallel_plan_t::terminal_t::AVG
        || terminal_ == parallel_plan_t::terminal_t::MIN
        || terminal_ == parallel_plan_t::terminal_t::MAX) {
        merge_partials(out);
        return;
    }
    switch (ordering_) {
    case parallel_plan_t::ordering_t::UNORDERED:
        drain_unordered(out);
        break;
    default:
        drain_ordered(out);
        break;
    }
}

void result_merger_t::drain_unordered(read_response_t *out) {
    std::vector<ql::datum_t> rows;
    for (auto &batch : unordered_queue_) {
        for (auto &row : batch.rows) {
            rows.push_back(std::move(row));
        }
        memory_used_bytes_ -= batch_cost(batch);
    }
    unordered_queue_.clear();
    if (memory_used_bytes_ < 0) {
        memory_used_bytes_ = 0;
    }
    assert_memory_invariants();
    build_stream_response(out, std::move(rows));
}

void result_merger_t::drain_ordered(read_response_t *out) {
    std::vector<ql::datum_t> rows;
    while (ordered_drain_ordinal_ < total_fragments_) {
        if (ordered_drain_ordinal_ < ordered_queues_.size()) {
            auto &q = ordered_queues_[ordered_drain_ordinal_];
            for (auto &batch : q) {
                for (auto &row : batch.rows) {
                    rows.push_back(std::move(row));
                }
                memory_used_bytes_ -= batch_cost(batch);
            }
            q.clear();
        }
        ++ordered_drain_ordinal_;
    }
    if (memory_used_bytes_ < 0) {
        memory_used_bytes_ = 0;
    }
    assert_memory_invariants();
    build_stream_response(out, std::move(rows));
}

void result_merger_t::merge_partials(read_response_t *out) {
    rget_read_response_t rget;

    if (partials_.empty()) {
        switch (terminal_) {
        case parallel_plan_t::terminal_t::COUNT: {
            ql::grouped_t<uint64_t> g;
            g[ql::datum_t()] = 0;
            rget.result = std::move(g);
            break;
        }
        case parallel_plan_t::terminal_t::SUM: {
            ql::grouped_t<double> g;
            g[ql::datum_t()] = 0.0;
            rget.result = std::move(g);
            break;
        }
        default: {
            ql::grouped_t<ql::stream_t> g;
            rget.result = std::move(g);
            break;
        }
        }
        out->response = std::move(rget);
        return;
    }

    switch (terminal_) {
    case parallel_plan_t::terminal_t::COUNT: {
        uint64_t total = 0;
        for (const auto &p : partials_) {
            if (p.state.has() && p.state.get_type() == ql::datum_t::R_NUM) {
                total += static_cast<uint64_t>(p.state.as_int());
            } else {
                total += static_cast<uint64_t>(p.input_rows);
            }
        }
        ql::grouped_t<uint64_t> g;
        g[ql::datum_t()] = total;
        rget.result = std::move(g);
        break;
    }
    case parallel_plan_t::terminal_t::SUM: {
        double total = 0.0;
        for (const auto &p : partials_) {
            if (p.state.has()) {
                total += p.state.as_num();
            }
        }
        ql::grouped_t<double> g;
        g[ql::datum_t()] = total;
        rget.result = std::move(g);
        break;
    }
    case parallel_plan_t::terminal_t::AVG: {
        double sum = 0.0;
        uint64_t count = 0;
        for (const auto &p : partials_) {
            if (p.state.has()) {
                sum += p.state.as_num();
            }
            count += static_cast<uint64_t>(p.input_rows);
        }
        ql::grouped_t<std::pair<double, uint64_t> > g;
        g[ql::datum_t()] = std::make_pair(sum, count);
        rget.result = std::move(g);
        break;
    }
    case parallel_plan_t::terminal_t::MIN:
    case parallel_plan_t::terminal_t::MAX: {
        ql::datum_t best;
        bool have = false;
        for (const auto &p : partials_) {
            if (!p.state.has()) {
                continue;
            }
            if (!have) {
                best = p.state;
                have = true;
            } else if (terminal_ == parallel_plan_t::terminal_t::MIN) {
                if (p.state < best) {
                    best = p.state;
                }
            } else if (p.state > best) {
                best = p.state;
            }
        }
        std::vector<ql::datum_t> rows;
        if (have) {
            rows.push_back(std::move(best));
        }
        build_stream_response(out, std::move(rows));
        partials_.clear();
        memory_used_bytes_ = 0;
        return;
    }
    default: {
        std::vector<ql::datum_t> rows;
        for (auto &p : partials_) {
            if (p.state.has()) {
                rows.push_back(std::move(p.state));
            }
        }
        build_stream_response(out, std::move(rows));
        partials_.clear();
        memory_used_bytes_ = 0;
        return;
    }
    }

    partials_.clear();
    memory_used_bytes_ = 0;
    out->response = std::move(rget);
}

// ── parallel_executor_t ────────────────────────────────────────────────────

parallel_executor_t::parallel_executor_t(store_t *store, const read_t &base_read,
    const parallel_plan_t &plan, const parallel_execution_limits_t &limits,
    signal_t *parent_interruptor)
    : store_(store),
      base_read_(base_read),
      plan_(plan),
      limits_(limits),
      parent_interruptor_(parent_interruptor),
      combined_interruptor_(parent_interruptor_, &canceled_cond_),
      merger_(plan.ordering(), plan.terminal(), plan.fragments().size(),
              limits.aggregate_buffer_bytes, &combined_interruptor_),
      active_workers_(0),
      next_fragment_index_(0),
      stop_launching_(false),
      latched_error_(false),
      error_fragment_ordinal_(SIZE_MAX) { }

parallel_executor_t::~parallel_executor_t() {
    canceled_cond_.pulse_if_not_already_pulsed();
}

void parallel_executor_t::fail_all(const query_exc_t &error,
                                   size_t fragment_ordinal) {
    assert_thread();
    if (latched_error_) {
        return;
    }
    latched_error_ = true;
    stop_launching_ = true;
    error_fragment_ordinal_ = fragment_ordinal;

    std::string msg = error.message();
    if (fragment_ordinal != SIZE_MAX) {
        msg += " (fragment ";
        msg += std::to_string(fragment_ordinal);
        msg += ")";
    }
    error_message_ = msg;

    merger_.fail(query_exc_t(msg));
    canceled_cond_.pulse_if_not_already_pulsed();
}

void parallel_executor_t::request_cancel(const interruption_t &reason) {
    assert_thread();
    if (!canceled_cond_.is_pulsed()) {
        fail_all(query_exc_t(reason.reason()), SIZE_MAX);
    }
}

void parallel_executor_t::run(read_response_t *response_out) {
    assert_thread();
    guarantee(response_out != nullptr);

    const auto &fragments = plan_.fragments();

    /* Aggregate timeout: feed signal_timer_t into the OR-interruptor (§7.3). */
    signal_timer_t aggregate_timer;
    if (limits_.aggregate_timeout_ms > 0) {
        aggregate_timer.start(limits_.aggregate_timeout_ms);
        combined_interruptor_.add(&aggregate_timer);
    }

    if (fragments.empty()) {
        rget_read_response_t rget;
        ql::grouped_t<ql::stream_t> empty;
        rget.result = std::move(empty);
        response_out->response = std::move(rget);
        return;
    }

#ifndef NDEBUG
    {
        std::set<size_t> seen;
        for (const auto &f : fragments) {
            rassert(f.ordinal() < fragments.size());
            rassert(seen.insert(f.ordinal()).second);
        }
    }
#endif

    size_t max_w = limits_.max_workers;
    if (max_w < 1) {
        max_w = 1;
    }
    if (max_w > server_parallel_workers_hard_max) {
        max_w = server_parallel_workers_hard_max;
    }
    if (max_w > fragments.size()) {
        max_w = fragments.size();
    }

    /* coro_t::spawn_sometime does not fail — true coro-pool exhaustion before
     * any output is a planner serial-fallback concern (Spec §7.5). */
    for (size_t i = 0; i < max_w && i < fragments.size() && !stop_launching_;
         ++i) {
        launch_worker(fragments[i]);
        ++next_fragment_index_;
    }

    {
        auto_drainer_t::lock_t keepalive(&drainer_);
        while (!merger_.all_fragments_complete()
               && !combined_interruptor_.is_pulsed()) {
            while (active_workers_ < max_w
                   && next_fragment_index_ < fragments.size()
                   && !stop_launching_
                   && !combined_interruptor_.is_pulsed()) {
                launch_worker(fragments[next_fragment_index_]);
                ++next_fragment_index_;
            }
            coro_t::yield();
        }
    }

    await_workers();

    if (aggregate_timer.is_pulsed() && !latched_error_) {
        std::string msg = "parallel query timed out after ";
        msg += std::to_string(limits_.aggregate_timeout_ms);
        msg += "ms";
        fail_all(query_exc_t(msg), SIZE_MAX);
    }

    if (latched_error_ && !error_message_.empty()) {
        throw cannot_perform_query_exc_t(error_message_, query_state_t::FAILED);
    }
    if (combined_interruptor_.is_pulsed() && !latched_error_) {
        throw interrupted_exc_t();
    }

#ifndef NDEBUG
    rassert(merger_.completed_count() == fragments.size()
            || merger_.failed());
    rassert(active_workers_ == 0);
#endif

    merger_.drain_into(response_out);
}

void parallel_executor_t::launch_worker(const query_fragment_t &fragment) {
    assert_thread();
    if (stop_launching_ || combined_interruptor_.is_pulsed()) {
        return;
    }
    ++active_workers_;
    auto_drainer_t::lock_t drain_lock(&drainer_);
    coro_t::spawn_sometime(
        [this, fragment, drain_lock = std::move(drain_lock)]() mutable {
            this->run_fragment(fragment, std::move(drain_lock));
        });
}

void parallel_executor_t::run_fragment(const query_fragment_t &fragment,
                                       auto_drainer_t::lock_t drain_lock) {
    assert_thread();
    const size_t ordinal = fragment.ordinal();

    signal_timer_t worker_timer;
    wait_any_t worker_interruptor(
        &combined_interruptor_, drain_lock.get_drain_signal());
    if (limits_.per_worker_timeout_ms > 0) {
        worker_timer.start(limits_.per_worker_timeout_ms);
        worker_interruptor.add(&worker_timer);
    }

    try {
        if (worker_interruptor.is_pulsed()) {
            throw interrupted_exc_t();
        }

        const key_range_t &range = fragment.input_range();
        store_key_t left = range.left;
        store_key_t right = range.right.unbounded
            ? store_key_t::max()
            : store_key_t(range.right.key());

        parallel_read_t pread(
            base_read_.get_region(),
            std::move(left),
            std::move(right),
            ordinal,
            std::vector<ql::transform_variant_t>(),
            r_nullopt,
            serializable_env_t());

        read_t frag_read(pread, base_read_.profile, base_read_.read_mode);
        read_response_t frag_resp;

        read_token_t token;
        store_->new_read_token(&token);

#ifndef NDEBUG
        metainfo_checker_t metainfo_checker(
            store_->get_region(),
            [](const region_t &, const binary_blob_t &) { });
#endif

        /* Wire through store_t::read → protocol_read + parallel_read_t handler
         * (store.cc, PAR-05/PAR-06). */
        store_->read(
            DEBUG_ONLY(metainfo_checker, )
            frag_read,
            &frag_resp,
            &token,
            &worker_interruptor);

        if (worker_timer.is_pulsed()) {
            std::string msg = "parallel worker timed out after ";
            msg += std::to_string(limits_.per_worker_timeout_ms);
            msg += "ms";
            fail_all(query_exc_t(msg), ordinal);
            merger_.mark_fragment_complete(ordinal);
            --active_workers_;
            return;
        }

        if (auto *rr = boost::get<rget_read_response_t>(&frag_resp.response)) {
            if (auto *ex = boost::get<ql::exc_t>(&rr->result)) {
                throw cannot_perform_query_exc_t(
                    ex->what(), query_state_t::FAILED);
            }

            if (auto *gs = boost::get<ql::grouped_t<ql::stream_t> >(&rr->result)) {
                fragment_batch_t batch;
                batch.fragment_ordinal = ordinal;
                batch.sequence_number = 0;
                batch.end_of_fragment = true;
                for (auto &group_pair : *gs->get_underlying_map()) {
                    for (auto &sub_pair : group_pair.second.substreams) {
                        for (auto &item : sub_pair.second.stream) {
                            batch.rows.push_back(std::move(item.data));
                        }
                    }
                }
                batch.encoded_bytes = batch_cost(batch);
                if (!batch.rows.empty()) {
                    merger_.push_batch(std::move(batch));
                }
            } else if (auto *gc =
                           boost::get<ql::grouped_t<uint64_t> >(&rr->result)) {
                partial_aggregate_t pa;
                pa.fragment_ordinal = ordinal;
                uint64_t total = 0;
                for (auto &p : *gc->get_underlying_map()) {
                    total += p.second;
                }
                pa.state = ql::datum_t(static_cast<double>(total));
                pa.input_rows = static_cast<int64_t>(total);
                merger_.push_partial(std::move(pa));
            } else if (auto *gd =
                           boost::get<ql::grouped_t<double> >(&rr->result)) {
                partial_aggregate_t pa;
                pa.fragment_ordinal = ordinal;
                double total = 0.0;
                for (auto &p : *gd->get_underlying_map()) {
                    total += p.second;
                }
                pa.state = ql::datum_t(total);
                pa.input_rows = 1;
                merger_.push_partial(std::move(pa));
            }
        }

        merger_.mark_fragment_complete(ordinal);
    } catch (const interrupted_exc_t &) {
        if (worker_timer.is_pulsed() && !latched_error_) {
            std::string msg = "parallel worker timed out after ";
            msg += std::to_string(limits_.per_worker_timeout_ms);
            msg += "ms";
            fail_all(query_exc_t(msg), ordinal);
        }
        merger_.mark_fragment_complete(ordinal);
    } catch (const cannot_perform_query_exc_t &e) {
        fail_all(query_exc_t(e.what()), ordinal);
        merger_.mark_fragment_complete(ordinal);
    } catch (const std::exception &e) {
        fail_all(query_exc_t(e.what()), ordinal);
        merger_.mark_fragment_complete(ordinal);
    } catch (...) {
        fail_all(query_exc_t("unknown parallel worker failure"), ordinal);
        merger_.mark_fragment_complete(ordinal);
    }

    guarantee(active_workers_ > 0);
    --active_workers_;
}

void parallel_executor_t::await_workers() {
    /* auto_drainer_t destructor joins workers. Explicit begin_draining here
     * would race with run()'s keepalive lock. */
    assert_thread();
}

// ── parallel_admission_token_t ──────────────────────────────────────────────

parallel_admission_token_t::parallel_admission_token_t(size_t workers, int64_t bytes)
    : workers_(workers), bytes_(bytes) { }
parallel_admission_token_t::~parallel_admission_token_t() { }
size_t parallel_admission_token_t::workers() const { return workers_; }
int64_t parallel_admission_token_t::bytes() const { return bytes_; }
