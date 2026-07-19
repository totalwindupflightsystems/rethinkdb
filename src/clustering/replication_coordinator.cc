#include "clustering/replication_coordinator.hpp"

namespace ql {

replication_coordinator_t::replication_coordinator_t(
    logical_log_retention_t *r) :
    cdc_perfmon_collection(),
    cdc_perfmon_membership(&get_global_perfmon_collection(),
                           &cdc_perfmon_collection, "cdc"),
    cdc_records_captured_membership(&cdc_perfmon_collection,
                                    &cdc_records_captured_total,
                                    "records_captured_total"),
    cdc_records_delivered_membership(&cdc_perfmon_collection,
                                     &cdc_records_delivered_total,
                                     "records_delivered_total"),
    cdc_delivery_latency_membership(&cdc_perfmon_collection,
                                    &cdc_delivery_latency_ms,
                                    "delivery_latency_ms"),
    cdc_slot_lag_bytes_membership(&cdc_perfmon_collection,
                                  &cdc_slot_lag_bytes,
                                  "slot_lag_bytes"),
    cdc_slot_lag_lsn_membership(&cdc_perfmon_collection,
                                &cdc_slot_lag_lsn,
                                "slot_lag_lsn"),
    cdc_retained_journal_bytes_membership(&cdc_perfmon_collection,
                                          &cdc_retained_journal_bytes,
                                          "retained_journal_bytes"),
    cdc_sink_retries_membership(&cdc_perfmon_collection,
                                &cdc_sink_retries_total,
                                "sink_retries_total"),
    cdc_sink_dead_letter_membership(&cdc_perfmon_collection,
                                    &cdc_sink_dead_letter_total,
                                    "sink_dead_letter_total"),
    cdc_resync_required_membership(&cdc_perfmon_collection,
                                   &cdc_resync_required_total,
                                   "resync_required_total"),
    cdc_records_captured_total(get_num_threads()),
    cdc_records_delivered_total(get_num_threads()),
    cdc_delivery_latency_ms(get_num_threads()),
    cdc_slot_lag_bytes(get_num_threads()),
    cdc_slot_lag_lsn(get_num_threads()),
    cdc_retained_journal_bytes(get_num_threads()),
    cdc_sink_retries_total(get_num_threads()),
    cdc_sink_dead_letter_total(get_num_threads()),
    cdc_resync_required_total(get_num_threads()),
    retention_(r) {}

void replication_coordinator_t::create_slot(
    const uuid_u &sid, const uuid_u &pid,
    const replication_consumer_identity_t &c) {
    std::lock_guard<std::mutex> lk(mutex_);
    replication_slot_info_t s;
    s.slot_id = sid; s.publication_id = pid; s.consumer = c;
    s.state = replication_slot_state_t::INITIALIZING;
    slots_[sid] = std::move(s);
}

void replication_coordinator_t::bind_slot(
    const uuid_u &sid, const publication_config_t &pub,
    const std::map<uuid_u, log_sequence_number_t> &base) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = slots_.find(sid);
    if (it == slots_.end()) return;
    publications_[pub.publication_id] = pub;
    for (auto &kv : base) {
        it->second.confirmed_lsn_by_shard[kv.first] = kv.second;
        retention_->pin_through(pub.table_id, kv.first, kv.second);
    }
    it->second.state = replication_slot_state_t::SNAPSHOTTING;
}

std::optional<replication_slot_info_t>
replication_coordinator_t::get_slot_state(const uuid_u &sid) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = slots_.find(sid);
    return it == slots_.end() ? std::nullopt
                              : std::optional(it->second);
}

void replication_coordinator_t::advance_slot(
    const uuid_u &sid, const shard_lsn_t &lsn, uint64_t inc) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = slots_.find(sid);
    if (it == slots_.end()) return;
    it->second.confirmed_lsn_by_shard[lsn.shard_id] = lsn.lsn;
    it->second.shard_incarnation_by_shard[lsn.shard_id] = inc;
    retention_->advance_slot(sid, lsn);
    if (it->second.state == replication_slot_state_t::SNAPSHOTTING)
        it->second.state = replication_slot_state_t::STREAMING;
}

bool replication_coordinator_t::note_flush_lsn(
    const uuid_u &sid, const shard_lsn_t &lsn, uint64_t inc) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = slots_.find(sid);
    if (it == slots_.end()) return false;
    it->second.flush_lsn_by_shard[lsn.shard_id] = lsn.lsn;
    it->second.shard_incarnation_by_shard[lsn.shard_id] = inc;
    return true;
}

bool replication_coordinator_t::confirm_lsn(
    const uuid_u &sid, const shard_lsn_t &lsn, uint64_t inc) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = slots_.find(sid);
    if (it == slots_.end()) return false;
    auto &ic = it->second.shard_incarnation_by_shard;
    auto ic_it = ic.find(lsn.shard_id);
    if (ic_it != ic.end() && ic_it->second != inc) return false;
    auto &cf = it->second.confirmed_lsn_by_shard;
    auto cf_it = cf.find(lsn.shard_id);
    if (cf_it != cf.end() && lsn.lsn.value <= cf_it->second.value) return false;
    auto &fl = it->second.flush_lsn_by_shard;
    auto fl_it = fl.find(lsn.shard_id);
    if (fl_it != fl.end() && lsn.lsn.value > fl_it->second.value) return false;
    advance_slot(sid, lsn, inc);
    return true;
}

void replication_coordinator_t::pause_slot(const uuid_u &sid) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = slots_.find(sid);
    if (it != slots_.end())
        it->second.state = replication_slot_state_t::PAUSED;
}

void replication_coordinator_t::resume_slot(const uuid_u &sid) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = slots_.find(sid);
    if (it != slots_.end())
        it->second.state = replication_slot_state_t::STREAMING;
}

void replication_coordinator_t::drop_slot(const uuid_u &sid) {
    std::lock_guard<std::mutex> lk(mutex_);
    retention_->release_slot(sid);
    slots_.erase(sid); bp_cfg_.erase(sid); bp_state_.erase(sid);
}

void replication_coordinator_t::evict_slot(
    const uuid_u &sid, const std::string &, const std::string &) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = slots_.find(sid);
    if (it == slots_.end()) return;
    it->second.state = replication_slot_state_t::EVICTED;
    retention_->release_slot(sid);
    ++cdc_resync_required_total;
}

void replication_coordinator_t::configure_backpressure(
    const uuid_u &sid, uint64_t maxf, uint64_t maxb) {
    std::lock_guard<std::mutex> lk(mutex_);
    bp_cfg_[sid] = {maxf, maxb}; bp_state_[sid] = {};
}

bool replication_coordinator_t::on_batch_enqueued(
    const uuid_u &sid, uint64_t b) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto &st = bp_state_[sid];
    auto ci = bp_cfg_.find(sid);
    if (ci == bp_cfg_.end()) return true;
    auto [mf, mb] = ci->second;
    if (st.in_flight_batches >= mf || st.buffered_bytes + b > mb) {
        st.source_paused = true; return false;
    }
    st.in_flight_batches++; st.buffered_bytes += b; return true;
}

void replication_coordinator_t::on_batch_dequeued(
    const uuid_u &sid, uint64_t b) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto &st = bp_state_[sid];
    if (st.in_flight_batches > 0) st.in_flight_batches--;
    st.buffered_bytes = (b >= st.buffered_bytes) ? 0 : st.buffered_bytes - b;
    auto ci = bp_cfg_.find(sid);
    if (ci != bp_cfg_.end()) {
        auto [mf, mb] = ci->second;
        if (st.in_flight_batches < mf && st.buffered_bytes <= mb)
            st.source_paused = false;
    }
}

slot_backpressure_t replication_coordinator_t::get_backpressure(
    const uuid_u &sid) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = bp_state_.find(sid);
    return it == bp_state_.end() ? slot_backpressure_t{} : it->second;
}

void replication_coordinator_t::note_journal_high_water(
    const uuid_u &tid, const uuid_u &shid,
    log_sequence_number_t lsn, uint64_t b, uint64_t ms) {
    std::lock_guard<std::mutex> lk(mutex_);
    journal_hw_[{tid, shid}] = {lsn, b, ms};
}

std::optional<slot_lag_t> replication_coordinator_t::get_slot_lag(
    const uuid_u &sid) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto si = slots_.find(sid);
    if (si == slots_.end()) return std::nullopt;
    auto pi = publications_.find(si->second.publication_id);
    if (pi == publications_.end()) return std::nullopt;
    slot_lag_t lag;
    for (auto &kv : si->second.confirmed_lsn_by_shard) {
        auto hi = journal_hw_.find({pi->second.table_id, kv.first});
        if (hi == journal_hw_.end()) continue;
        if (hi->second.bytes > lag.lag_bytes) lag.lag_bytes = hi->second.bytes;
        if (hi->second.lsn.value > kv.second.value) {
            uint64_t lsn_lag = hi->second.lsn.value - kv.second.value;
            if (lsn_lag > lag.lag_lsn) lag.lag_lsn = lsn_lag;
        }
    }
    lag.warn_threshold_breached =
        lag.lag_bytes >= pi->second.max_slot_lag_bytes * 80 / 100;
    lag.hard_threshold_breached =
        lag.lag_bytes >= pi->second.max_slot_lag_bytes;
    // CDC-08f: cumulative lag tracking — counters increase with observed lag
    cdc_slot_lag_bytes += lag.lag_bytes;
    cdc_slot_lag_lsn += lag.lag_lsn;
    if (lag.hard_threshold_breached)
        const_cast<replication_coordinator_t*>(this)->pause_slot(sid);
    return lag;
}

void replication_coordinator_t::on_shard_routing_change(
    const uuid_u &, std::tuple<uuid_u, uint64_t, uint64_t> ch) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto [shid, inc, maxr] = ch; (void)maxr;
    for (auto &kv : slots_)
        kv.second.shard_incarnation_by_shard[shid] = inc;
}

void replication_coordinator_t::record_captured(uint64_t count) {
    cdc_records_captured_total += count;
}

void replication_coordinator_t::record_delivered(uint64_t count) {
    cdc_records_delivered_total += count;
}

void replication_coordinator_t::record_delivery_latency(uint64_t ms) {
    cdc_delivery_latency_ms += ms;
}

void replication_coordinator_t::record_sink_retry() {
    ++cdc_sink_retries_total;
}

void replication_coordinator_t::record_sink_dead_letter() {
    ++cdc_sink_dead_letter_total;
}

void replication_coordinator_t::record_resync_required() {
    ++cdc_resync_required_total;
}

}  // namespace ql
