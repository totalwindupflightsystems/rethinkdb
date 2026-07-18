// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/parallel_executor.hpp"

#include "arch/runtime/coroutines.hpp"
#include "concurrency/interruptor.hpp"
#include "rdb_protocol/btree.hpp"
#include "rdb_protocol/env.hpp"
#include "rdb_protocol/protocol.hpp"
#include "rdb_protocol/store.hpp"

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
const std::vector<query_fragment_t> &parallel_plan_t::fragments() const { return fragments_; }
auto parallel_plan_t::ordering() const -> ordering_t { return ordering_; }
auto parallel_plan_t::terminal() const -> terminal_t { return terminal_; }
bool parallel_plan_t::preserves_serial_semantics() const { return true; }
int64_t parallel_plan_t::estimated_serial_us() const { return estimated_serial_us_; }
int64_t parallel_plan_t::estimated_parallel_us() const { return estimated_parallel_us_; }

// ── fragment_result_t ───────────────────────────────────────────────────────

fragment_result_t::fragment_result_t(state_t state)
    : state_(state), complete_ordinal_(0) { }
auto fragment_result_t::state() const -> state_t { return state_; }
const fragment_batch_t &fragment_result_t::batch() const { return batch_; }
const partial_aggregate_t &fragment_result_t::partial() const { return partial_; }
size_t fragment_result_t::complete_ordinal() const { return complete_ordinal_; }
const query_exc_t &fragment_result_t::error() const { return error_; }

fragment_result_t fragment_result_t::make_batch(fragment_batch_t batch) {
    fragment_result_t r(state_t::BATCH); r.batch_ = std::move(batch); return r;
}
fragment_result_t fragment_result_t::make_partial(partial_aggregate_t partial) {
    fragment_result_t r(state_t::PARTIAL); r.partial_ = std::move(partial); return r;
}
fragment_result_t fragment_result_t::make_complete(size_t ordinal) {
    fragment_result_t r(state_t::COMPLETE); r.complete_ordinal_ = ordinal; return r;
}
fragment_result_t fragment_result_t::make_error(query_exc_t error) {
    fragment_result_t r(state_t::ERROR); r.error_ = std::move(error); return r;
}
fragment_result_t fragment_result_t::make_canceled() {
    return fragment_result_t(state_t::CANCELED);
}

// ── result_merger_t ─────────────────────────────────────────────────────────

static int64_t batch_cost(const fragment_batch_t &b) {
    return b.encoded_bytes > 0 ? b.encoded_bytes
                               : static_cast<int64_t>(b.rows.size()) * 256;
}

result_merger_t::result_merger_t(parallel_plan_t::ordering_t ordering,
    parallel_plan_t::terminal_t terminal, size_t total_fragments,
    int64_t memory_limit_bytes, signal_t *interruptor)
    : ordering_(ordering), terminal_(terminal), total_fragments_(total_fragments),
      memory_limit_bytes_(memory_limit_bytes), memory_used_bytes_(0),
      interruptor_(interruptor), ordered_drain_ordinal_(0), failed_(false) { }

result_merger_t::~result_merger_t() { }

bool result_merger_t::all_fragments_complete() const {
    return completed_fragments_.size() == total_fragments_ || failed_;
}

void result_merger_t::push_batch(fragment_batch_t batch) {
    if (failed_) return;
    int64_t cost = batch_cost(batch);
    while (memory_limit_bytes_ > 0
           && memory_used_bytes_ + cost > memory_limit_bytes_
           && !failed_ && !interruptor_->is_pulsed()) {
        wait_any_t w(&space_available_, interruptor_);
        w.wait_lazily_unordered();
    }
    if (failed_ || interruptor_->is_pulsed()) return;
    memory_used_bytes_ += cost;
    if (ordering_ == parallel_plan_t::ordering_t::UNORDERED)
        unordered_queue_.push_back(std::move(batch));
    else
        ordered_queues_[batch.fragment_ordinal].push_back(std::move(batch));
}

void result_merger_t::push_partial(partial_aggregate_t partial) {
    if (!failed_) partials_.push_back(std::move(partial));
}

void result_merger_t::mark_fragment_complete(size_t ordinal) {
    if (!failed_) {
        completed_fragments_.insert(ordinal);
        space_available_.pulse_if_not_already_pulsed();
    }
}

void result_merger_t::fail(const query_exc_t &) {
    failed_ = true;
    space_available_.pulse_if_not_already_pulsed();
}

void result_merger_t::drain_into(read_response_t *out) {
    guarantee(out != nullptr);
    if (!partials_.empty()) { merge_partials(out); return; }
    switch (ordering_) {
    case parallel_plan_t::ordering_t::UNORDERED: drain_unordered(out); break;
    default: drain_ordered(out); break;
    }
}

void result_merger_t::drain_unordered(read_response_t *out) {
    out->response = rget_read_response_t();
    auto *res = boost::get<rget_read_response_t>(&out->response);
    res->result = ql::wire_datum_map_t();
    auto &rows = boost::get<ql::wire_datum_map_t>(res->result);
    for (auto &batch : unordered_queue_) {
        if (!batch.end_of_fragment)
            for (auto &row : batch.rows)
                rows.emplace_back(std::vector<ql::datum_t>{std::move(row)});
        memory_used_bytes_ -= batch_cost(batch);
    }
    unordered_queue_.clear();
}

void result_merger_t::drain_ordered(read_response_t *out) {
    out->response = rget_read_response_t();
    auto *res = boost::get<rget_read_response_t>(&out->response);
    res->result = ql::wire_datum_map_t();
    auto &rows = boost::get<ql::wire_datum_map_t>(res->result);
    while (ordered_drain_ordinal_ < total_fragments_) {
        auto it = ordered_queues_.find(ordered_drain_ordinal_);
        if (it == ordered_queues_.end()) { ++ordered_drain_ordinal_; continue; }
        for (auto &batch : it->second) {
            if (!batch.end_of_fragment)
                for (auto &row : batch.rows)
                    rows.emplace_back(std::vector<ql::datum_t>{std::move(row)});
            memory_used_bytes_ -= batch_cost(batch);
        }
        ordered_queues_.erase(it);
        ++ordered_drain_ordinal_;
    }
}

void result_merger_t::merge_partials(read_response_t *out) {
    out->response = rget_read_response_t();
    auto *res = boost::get<rget_read_response_t>(&out->response);
    if (partials_.empty()) { res->result = ql::datum_t(ql::datum_t::R_NULL); return; }
    switch (terminal_) {
    case parallel_plan_t::terminal_t::COUNT:
    case parallel_plan_t::terminal_t::SUM: {
        ql::datum_t accum = partials_[0].state;
        for (size_t i = 1; i < partials_.size(); ++i)
            accum = ql::datum_t(accum.as_num() + partials_[i].state.as_num());
        res->result = accum; break;
    }
    case parallel_plan_t::terminal_t::MIN: {
        ql::datum_t best = partials_[0].state;
        for (size_t i = 1; i < partials_.size(); ++i)
            if (partials_[i].state.as_num() < best.as_num()) best = partials_[i].state;
        res->result = best; break;
    }
    case parallel_plan_t::terminal_t::MAX: {
        ql::datum_t best = partials_[0].state;
        for (size_t i = 1; i < partials_.size(); ++i)
            if (partials_[i].state.as_num() > best.as_num()) best = partials_[i].state;
        res->result = best; break;
    }
    case parallel_plan_t::terminal_t::AVG: {
        double sum = 0; int64_t count = 0;
        for (auto &p : partials_) { sum += p.state.as_num(); count += p.input_rows; }
        res->result = count > 0 ? ql::datum_t(sum / static_cast<double>(count))
                                : ql::datum_t(ql::datum_t::R_NULL);
        break;
    }
    default:
        res->result = ql::wire_datum_map_t();
        auto &rows = boost::get<ql::wire_datum_map_t>(res->result);
        for (auto &p : partials_)
            rows.emplace_back(std::vector<ql::datum_t>{p.state});
        break;
    }
}

// ── parallel_executor_t ────────────────────────────────────────────────────

parallel_executor_t::parallel_executor_t(store_t *store, const read_t &base_read,
    const parallel_plan_t &plan, const parallel_execution_limits_t &limits,
    signal_t *parent_interruptor)
    : store_(store), base_read_(base_read), plan_(plan), limits_(limits),
      parent_interruptor_(parent_interruptor),
      combined_interruptor_(parent_interruptor_, &canceled_cond_),
      merger_(plan.ordering(), plan.terminal(), plan.fragments().size(),
              limits.aggregate_buffer_bytes, &combined_interruptor_),
      active_workers_(0), next_fragment_index_(0), latched_error_(false) { }

parallel_executor_t::~parallel_executor_t() { }

void parallel_executor_t::request_cancel(const interruption_t &reason) {
    assert_thread();
    if (!canceled_cond_.is_pulsed()) {
        error_message_ = reason.reason();
        latched_error_ = true;
        merger_.fail(query_exc_t(reason.reason()));
        canceled_cond_.pulse();
    }
}

void parallel_executor_t::run(read_response_t *response_out) {
    assert_thread();
    guarantee(response_out != nullptr);
    const auto &fragments = plan_.fragments();
    if (fragments.empty()) {
        response_out->response = rget_read_response_t();
        auto *res = boost::get<rget_read_response_t>(&response_out->response);
        res->result = ql::datum_t::empty_array();
        return;
    }
    size_t max_w = limits_.max_workers;
    if (max_w < 1) max_w = 1;
    if (max_w > fragments.size()) max_w = fragments.size();
    for (size_t i = 0; i < max_w && i < fragments.size(); ++i) {
        launch_worker(fragments[i]); ++next_fragment_index_;
    }
    {
        auto_drainer_t::lock_t keepalive(&drainer_);
        while (!merger_.all_fragments_complete()
               && !combined_interruptor_.is_pulsed()) {
            while (active_workers_ < max_w
                   && next_fragment_index_ < fragments.size()
                   && !combined_interruptor_.is_pulsed()) {
                launch_worker(fragments[next_fragment_index_]);
                ++next_fragment_index_;
            }
            coro_t::yield();
        }
    }
    if (latched_error_ && !error_message_.empty())
        throw cannot_perform_query_exc_t(error_message_, query_state_t::FAILED);
    if (combined_interruptor_.is_pulsed() && !latched_error_)
        throw interrupted_exc_t();
    merger_.drain_into(response_out);
}

void parallel_executor_t::launch_worker(const query_fragment_t &fragment) {
    assert_thread();
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
    try {
        const key_range_t &range = fragment.input_range();
        store_key_t left = range.left;
        store_key_t right = range.right.is_unbounded()
            ? store_key_t::max() : store_key_t(range.right.key());
        parallel_read_t pread(base_read_.region, std::move(left),
            std::move(right), fragment.ordinal(), {}, r_nullopt,
            serializable_env_t());
        read_t frag_read(pread, base_read_.profile, base_read_.read_mode);
        read_response_t frag_resp;
        store_->protocol_read(frag_read, &frag_resp,
                              drain_lock.get_drain_signal());
        if (auto *rr = boost::get<rget_read_response_t>(&frag_resp.response)) {
            if (auto *wm = boost::get<ql::wire_datum_map_t>(&rr->result)) {
                fragment_batch_t batch;
                batch.fragment_ordinal = fragment.ordinal();
                batch.sequence_number = 0;
                for (auto &rv : *wm)
                    for (auto &d : rv) batch.rows.push_back(std::move(d));
                batch.end_of_fragment = true;
                if (!batch.rows.empty()) merger_.push_batch(std::move(batch));
            } else if (auto *d = boost::get<ql::datum_t>(&rr->result)) {
                partial_aggregate_t pa;
                pa.fragment_ordinal = fragment.ordinal();
                pa.state = *d; pa.input_rows = 1;
                merger_.push_partial(std::move(pa));
            }
        }
        merger_.mark_fragment_complete(fragment.ordinal());
    } catch (const interrupted_exc_t &) {
        merger_.mark_fragment_complete(fragment.ordinal());
    } catch (const cannot_perform_query_exc_t &e) {
        if (!latched_error_) {
            error_message_ = e.what();
            latched_error_ = true;
            merger_.fail(query_exc_t(e.what()));
            canceled_cond_.pulse();
        }
        merger_.mark_fragment_complete(fragment.ordinal());
    } catch (const std::exception &e) {
        if (!latched_error_) {
            error_message_ = e.what();
            latched_error_ = true;
            merger_.fail(query_exc_t(e.what()));
            canceled_cond_.pulse();
        }
        merger_.mark_fragment_complete(fragment.ordinal());
    }
    --active_workers_;
}

void parallel_executor_t::await_workers() { }

// ── parallel_admission_token_t ──────────────────────────────────────────────

parallel_admission_token_t::parallel_admission_token_t(size_t workers, int64_t bytes)
    : workers_(workers), bytes_(bytes) { }
parallel_admission_token_t::~parallel_admission_token_t() { }
size_t parallel_admission_token_t::workers() const { return workers_; }
int64_t parallel_admission_token_t::bytes() const { return bytes_; }
