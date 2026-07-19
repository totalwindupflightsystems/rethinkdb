#include "serializer/log/lba/logical_log_retention.hpp"

namespace ql {

void logical_log_retention_t::pin_through(
    const uuid_u &sid, const uuid_u &tid, const uuid_u &shid,
    log_sequence_number_t lsn) {
    std::lock_guard<std::mutex> lk(mutex_);
    key_t k{tid, shid};
    slot_pins_[sid][k] = lsn;
    // Recalculate aggregate floor = MIN across all slots for this key
    log_sequence_number_t min_lsn{0};
    bool first = true;
    for (auto &sp : slot_pins_) {
        auto pk = sp.second.find(k);
        if (pk != sp.second.end()) {
            if (first || pk->second.value < min_lsn.value) {
                min_lsn = pk->second;
                first = false;
            }
        }
    }
    if (!first)
        floors_[k] = min_lsn;
}

void logical_log_retention_t::advance_slot(
    const uuid_u &sid, const shard_lsn_t &shard_lsn) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto si = slot_pins_.find(sid);
    if (si == slot_pins_.end()) return;
    // Find the matching key via the slot's shard_id
    key_t matching_key;
    bool found = false;
    for (auto &pk : si->second) {
        if (pk.first.shard_id == shard_lsn.shard_id) {
            matching_key = pk.first;
            found = true;
            break;
        }
    }
    if (!found) return;
    si->second[matching_key] = shard_lsn.lsn;
    // Recalculate aggregate floor = MIN across all slots for this key
    log_sequence_number_t min_lsn{0};
    bool first = true;
    for (auto &sp : slot_pins_) {
        auto pk = sp.second.find(matching_key);
        if (pk != sp.second.end()) {
            if (first || pk->second.value < min_lsn.value) {
                min_lsn = pk->second;
                first = false;
            }
        }
    }
    floors_[matching_key] = first ? log_sequence_number_t{0} : min_lsn;
}

void logical_log_retention_t::release_slot(const uuid_u &sid) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto si = slot_pins_.find(sid);
    if (si == slot_pins_.end()) return;
    // Collect keys this slot pinned before erasing
    std::vector<key_t> affected_keys;
    for (auto &pk : si->second)
        affected_keys.push_back(pk.first);
    slot_pins_.erase(si);
    // Recalculate aggregate floor = MIN across remaining slots for each key
    for (auto &k : affected_keys) {
        log_sequence_number_t min_lsn{0};
        bool first = true;
        for (auto &sp : slot_pins_) {
            auto pk = sp.second.find(k);
            if (pk != sp.second.end()) {
                if (first || pk->second.value < min_lsn.value) {
                    min_lsn = pk->second;
                    first = false;
                }
            }
        }
        if (first)
            floors_.erase(k);
        else
            floors_[k] = min_lsn;
    }
}

log_sequence_number_t logical_log_retention_t::retention_floor(
    const uuid_u &tid, const uuid_u &shid) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = floors_.find(key_t{tid, shid});
    return it == floors_.end() ? log_sequence_number_t{0} : it->second;
}

}  // namespace ql
