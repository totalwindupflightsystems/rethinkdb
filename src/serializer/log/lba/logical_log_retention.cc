#include "serializer/log/lba/logical_log_retention.hpp"

namespace ql {

void logical_log_retention_t::pin_through(
    const uuid_u &tid, const uuid_u &shid, log_sequence_number_t lsn) {
    std::lock_guard<std::mutex> lk(mutex_);
    key_t k{tid, shid};
    auto it = floors_.find(k);
    if (it == floors_.end() || lsn.value > it->second.value)
        floors_[k] = lsn;
}
void logical_log_retention_t::advance_slot(
    const uuid_u &, const shard_lsn_t &) {}
void logical_log_retention_t::release_slot(const uuid_u &) {}
log_sequence_number_t logical_log_retention_t::retention_floor(
    const uuid_u &tid, const uuid_u &shid) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = floors_.find(key_t{tid, shid});
    return it == floors_.end() ? log_sequence_number_t{0} : it->second;
}

}  // namespace ql
