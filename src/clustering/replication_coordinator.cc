// Copyright 2026 RethinkDB, all rights reserved.
#include "clustering/replication_coordinator.hpp"
#include <algorithm>
#include <optional>
#include <stdexcept>
#include "arch/runtime/runtime.hpp"
#include "containers/archive/archive.hpp"
#include "containers/archive/stl_types.hpp"
#include "containers/archive/versioned.hpp"
#include "errors.hpp"

namespace ql {

RDB_IMPL_SERIALIZABLE_4_SINCE_v2_4(
    ql::replication_consumer_identity_t,
    consumer_id, credential_ref, supported_schemas, protocol_version);

// 20-field manual serialization (exceeds 19-field macro ceiling)
template <cluster_version_t W>
void serialize(write_message_t *wm, const ql::replication_slot_record_t &t) {
    serialize<W>(wm, t.slot_id);
    serialize<W>(wm, t.publication_id);
    serialize<W>(wm, t.table_id);
    serialize<W>(wm, t.consumer);
    serialize<W>(wm, t.state);
    serialize<W>(wm, t.confirmed_lsn_by_shard);
    serialize<W>(wm, t.flush_lsn_by_shard);
    serialize<W>(wm, t.snapshot_barrier_lsn_by_shard);
    serialize<W>(wm, t.shard_incarnation_by_shard);
    serialize<W>(wm, t.last_ack_at);
    serialize<W>(wm, t.retained_bytes);
    serialize<W>(wm, t.lag_bytes);
    serialize<W>(wm, t.lag_lsn);
    serialize<W>(wm, t.lag_ms);
    serialize<W>(wm, t.lag_warn_bytes);
    serialize<W>(wm, t.lag_hard_bytes);
    serialize<W>(wm, t.last_error_code);
    serialize<W>(wm, t.last_error_message);
    serialize<W>(wm, t.last_error_at);
    serialize<W>(wm, t.created_at);
}
template <cluster_version_t W>
archive_result_t deserialize(read_stream_t *s, ql::replication_slot_record_t *t) {
    archive_result_t r = archive_result_t::SUCCESS;
    #define D(f) if(bad(r=deserialize<W>(s,deserialize_deref(t->f))))return r
    D(slot_id); D(publication_id); D(table_id); D(consumer); D(state);
    D(confirmed_lsn_by_shard); D(flush_lsn_by_shard);
    D(snapshot_barrier_lsn_by_shard); D(shard_incarnation_by_shard);
    D(last_ack_at); D(retained_bytes); D(lag_bytes); D(lag_lsn); D(lag_ms);
    D(lag_warn_bytes); D(lag_hard_bytes); D(last_error_code);
    D(last_error_message); D(last_error_at); D(created_at);
    #undef D
    return r;
}
INSTANTIATE_SERIALIZABLE_SINCE_v2_4(ql::replication_slot_record_t);

namespace {
constexpr double LAG_WARN = 0.8;
constexpr uint64_t DEF_BATCHES = 4;
constexpr uint64_t DEF_BUF = 16ULL*1024*1024;
} // namespace

replication_coordinator_t::replication_coordinator_t(
        logical_log_retention_t *r)
    : home_thread_mixin_t(get_thread_id()), retention_(r) {
    guarantee(retention_ != nullptr, "coordinator needs retention tracker");
}
replication_coordinator_t::~replication_coordinator_t() {
    assert_thread(); drainer_.begin_draining();
    for (auto &kv : slots_) retention_->release_slot(kv.first);
    drainer_.drain();
}

void replication_coordinator_t::create_slot(
        const uuid_u &sid, const uuid_u &pid,
        const replication_consumer_identity_t &c) {
    assert_thread(); std::lock_guard<std::mutex> lk(mutex_);
    if (slots_.count(sid))
        throw std::runtime_error("slot exists: "+uuid_to_str(sid));
    replication_slot_record_t r;
    r.slot_id=sid; r.publication_id=pid; r.consumer=c;
    r.state=replication_slot_state_t::CREATING;
    r.created_at=current_microtime();
    slots_[sid]=r;
    backpressure_[sid]=slot_backpressure_t{DEF_BATCHES,DEF_BUF,DEF_BATCHES,DEF_BUF};
}

void replication_coordinator_t::bind_slot(
        const uuid_u &sid, const publication_config_t &pub,
        const std::map<uuid_u,log_sequence_number_t> &barriers) {
    assert_thread(); std::lock_guard<std::mutex> lk(mutex_);
    auto it=slots_.find(sid);
    if(it==slots_.end())
        throw std::runtime_error("bind unknown slot: "+uuid_to_str(sid));
    auto &s=it->second;
    s.table_id=pub.table_id;
    s.snapshot_barrier_lsn_by_shard=barriers;
    s.lag_warn_bytes=static_cast<uint64_t>(pub.max_slot_lag_bytes*LAG_WARN);
    s.lag_hard_bytes=pub.max_slot_lag_bytes;
    transition_locked(sid,replication_slot_state_t::CONNECTING);
    if(pub.default_snapshot_mode==snapshot_mode_t::FULL && !barriers.empty())
        transition_locked(sid,replication_slot_state_t::SNAPSHOTTING);
    else transition_locked(sid,replication_slot_state_t::CATCHING_UP);
    for(auto &kv:barriers)
        retention_->pin_through(sid, s.table_id, kv.first, kv.second);
}

void replication_coordinator_t::advance_slot(
        const uuid_u &sid, const shard_lsn_t &con, uint64_t inc) {
    assert_thread(); std::lock_guard<std::mutex> lk(mutex_);
    auto it=slots_.find(sid);
    if(it==slots_.end()) return;
    auto &s=it->second;

    auto ii=s.shard_incarnation_by_shard.find(con.shard_id);
    if(ii!=s.shard_incarnation_by_shard.end() && ii->second!=inc) {
        s.last_error_code="STALE_SHARD";
        s.last_error_message="stale incarnation ACK";
        s.last_error_at=current_microtime();
        return;
    }
    if(ii==s.shard_incarnation_by_shard.end())
        s.shard_incarnation_by_shard[con.shard_id]=inc;

    auto ci=s.confirmed_lsn_by_shard.find(con.shard_id);
    if(ci!=s.confirmed_lsn_by_shard.end() && con.lsn<=ci->second) return;

    s.confirmed_lsn_by_shard[con.shard_id]=con.lsn;
    s.last_ack_at=current_microtime();
    retention_->advance_slot(sid, con);

    if(s.state==replication_slot_state_t::SNAPSHOTTING
       || s.state==replication_slot_state_t::CATCHING_UP) {
        bool all=true;
        for(auto &kv:s.snapshot_barrier_lsn_by_shard) {
            auto c=s.confirmed_lsn_by_shard.find(kv.first);
            if(c==s.confirmed_lsn_by_shard.end() || c->second<kv.second)
            { all=false; break; }
        }
        if(all && s.state==replication_slot_state_t::SNAPSHOTTING)
            transition_locked(sid,replication_slot_state_t::CATCHING_UP);
        if(all && s.state==replication_slot_state_t::CATCHING_UP)
            transition_locked(sid,replication_slot_state_t::STREAMING);
    }
    recompute_lag_locked(&s);
}

void replication_coordinator_t::pause_slot(const uuid_u &sid) {
    assert_thread(); std::lock_guard<std::mutex> lk(mutex_);
    auto it=slots_.find(sid);
    if(it==slots_.end()) return;
    auto &s=it->second;
    if(s.state==replication_slot_state_t::ERROR
       || s.state==replication_slot_state_t::DROPPED) return;
    transition_locked(sid,replication_slot_state_t::PAUSED);
    auto bp=backpressure_.find(sid);
    if(bp!=backpressure_.end()) bp->second.source_paused=true;
}
void replication_coordinator_t::resume_slot(const uuid_u &sid) {
    assert_thread(); std::lock_guard<std::mutex> lk(mutex_);
    auto it=slots_.find(sid);
    if(it==slots_.end()) return;
    if(it->second.state!=replication_slot_state_t::PAUSED) return;
    bool all=true;
    for(auto &kv:it->second.snapshot_barrier_lsn_by_shard) {
        auto c=it->second.confirmed_lsn_by_shard.find(kv.first);
        if(c==it->second.confirmed_lsn_by_shard.end() || c->second<kv.second)
        { all=false; break; }
    }
    transition_locked(sid,all?replication_slot_state_t::STREAMING
                               :replication_slot_state_t::CATCHING_UP);
    auto bp=backpressure_.find(sid);
    if(bp!=backpressure_.end()) bp->second.source_paused=false;
}
void replication_coordinator_t::drop_slot(const uuid_u &sid) {
    assert_thread(); std::lock_guard<std::mutex> lk(mutex_);
    if(!slots_.count(sid)) return;
    release_retention_locked(sid);
    slots_.erase(sid); backpressure_.erase(sid);
}
void replication_coordinator_t::record_slot_error(
        const uuid_u &sid, const std::string &c, const std::string &m) {
    assert_thread(); std::lock_guard<std::mutex> lk(mutex_);
    auto it=slots_.find(sid);
    if(it==slots_.end()) return;
    it->second.last_error_code=c;
    it->second.last_error_message=m;
    it->second.last_error_at=current_microtime();
    transition_locked(sid,replication_slot_state_t::ERROR);
}

optional<replication_slot_record_t>
replication_coordinator_t::get_slot_state(const uuid_u &sid) const {
    assert_thread(); std::lock_guard<std::mutex> lk(mutex_);
    auto it=slots_.find(sid);
    return it==slots_.end()?optional<replication_slot_record_t>():optional<replication_slot_record_t>(it->second);
}
slot_backpressure_t replication_coordinator_t::get_backpressure(
        const uuid_u &sid) const {
    assert_thread(); std::lock_guard<std::mutex> lk(mutex_);
    auto it=backpressure_.find(sid);
    return it==backpressure_.end()?slot_backpressure_t{}:it->second;
}
std::vector<replication_slot_record_t>
replication_coordinator_t::list_slots() const {
    assert_thread(); std::lock_guard<std::mutex> lk(mutex_);
    std::vector<replication_slot_record_t> v; v.reserve(slots_.size());
    for(auto &kv:slots_) v.push_back(kv.second);
    return v;
}
size_t replication_coordinator_t::streaming_slot_count() const {
    assert_thread(); std::lock_guard<std::mutex> lk(mutex_);
    size_t n=0;
    for(auto &kv:slots_)
        if(kv.second.state==replication_slot_state_t::STREAMING) ++n;
    return n;
}
uint64_t replication_coordinator_t::total_retained_bytes() const {
    assert_thread(); std::lock_guard<std::mutex> lk(mutex_);
    uint64_t t=0;
    for(auto &kv:slots_) t+=kv.second.retained_bytes;
    return t;
}

bool replication_coordinator_t::on_batch_enqueued(
        const uuid_u &sid, uint64_t b) {
    assert_thread(); std::lock_guard<std::mutex> lk(mutex_);
    auto it=backpressure_.find(sid);
    if(it==backpressure_.end()) return false;
    auto &bp=it->second;
    bp.in_flight_batches++; bp.buffered_bytes+=b;
    return bp.source_paused?false:!bp.at_capacity();
}
void replication_coordinator_t::on_batch_dequeued(
        const uuid_u &sid, uint64_t b) {
    assert_thread(); std::lock_guard<std::mutex> lk(mutex_);
    auto it=backpressure_.find(sid);
    if(it==backpressure_.end()) return;
    auto &bp=it->second;
    if(bp.in_flight_batches>0) bp.in_flight_batches--;
    bp.buffered_bytes=(b>=bp.buffered_bytes)?0:bp.buffered_bytes-b;
}

optional<slot_lag_t>
replication_coordinator_t::get_slot_lag(const uuid_u &sid) const {
    assert_thread(); std::lock_guard<std::mutex> lk(mutex_);
    auto it=slots_.find(sid);
    if(it==slots_.end()) return optional<slot_lag_t>();
    auto &s=it->second;
    slot_lag_t lag;
    lag.lag_bytes=s.lag_bytes;
    lag.lag_lsn=s.lag_lsn;
    lag.lag_ms=s.lag_ms;
    lag.warn_threshold_breached=(s.lag_bytes > s.lag_warn_bytes && s.lag_warn_bytes > 0);
    lag.hard_threshold_breached=(s.lag_bytes > s.lag_hard_bytes && s.lag_hard_bytes > 0);
    return optional<slot_lag_t>(lag);
}

void replication_coordinator_t::on_shard_routing_change(
        const uuid_u &pid, const shard_routing_event_t &ev) {
    assert_thread(); std::lock_guard<std::mutex> lk(mutex_);
    for(auto &kv:slots_) {
        auto &s=kv.second;
        if(s.publication_id!=pid) continue;
        s.shard_incarnation_by_shard[ev.shard_id]=ev.new_incarnation;
        if(s.state==replication_slot_state_t::STREAMING)
            transition_locked(s.slot_id,replication_slot_state_t::CONNECTING);
    }
}

void replication_coordinator_t::transition_locked(
        const uuid_u &sid, replication_slot_state_t ns) {
    auto it = slots_.find(sid);
    if (it != slots_.end()) it->second.state = ns;
}
void replication_coordinator_t::release_retention_locked(const uuid_u &sid) {
    retention_->release_slot(sid);
}
void replication_coordinator_t::recompute_lag_locked(
        replication_slot_record_t *s) {
    constexpr uint64_t AVG=256;
    uint64_t gap=0;
    for(auto &kv:s->flush_lsn_by_shard) {
        auto c=s->confirmed_lsn_by_shard.find(kv.first);
        if(c==s->confirmed_lsn_by_shard.end()) gap+=kv.second.value;
        else if(kv.second.value>c->second.value)
            gap+=(kv.second.value-c->second.value);
    }
    s->lag_lsn=gap; s->lag_bytes=gap*AVG;
    if(s->last_ack_at>0) {
        auto now=current_microtime();
        s->lag_ms=(now>s->last_ack_at)?((now-s->last_ack_at)/1000):0;
    }
}

}  // namespace ql
