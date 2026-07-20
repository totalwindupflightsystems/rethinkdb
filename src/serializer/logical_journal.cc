// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "serializer/logical_journal.hpp"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "arch/io/disk.hpp"
#include "logger.hpp"
#include "paths.hpp"
#include "utils.hpp"

// ── Constants ────────────────────────────────────────────────────────
//
// The on-disk record framing is fixed by CDC-04. Future revisions
// bump RECORD_FORMAT_VERSION and add a feature switch here.

namespace {

constexpr const char *INDEX_MAGIC = "RDBJIDX1";  // 8 bytes, no NUL
constexpr size_t INDEX_HEADER_SIZE = 8;
constexpr size_t INDEX_ENTRY_SIZE = 28;  // 8+8+8+4 LE fields

// File mode bits for journal files. World-readable would be unusual
// here, but on-disk data is not encrypted and lives in the
// serializer directory which has its own permissions; 0644 mirrors
// the convention used by the log writer.
constexpr mode_t FILE_MODE = 0644;

constexpr mode_t DIR_MODE = 0755;

// ── Little-endian helpers ────────────────────────────────────────────
//
// We use memcpy-based LE helpers rather than reinterpret_cast to
// stay portable across host endianness and to avoid strict-aliasing
// warnings in debug builds.

inline void put_u8(std::vector<char> *out, uint8_t v) {
    out->push_back(static_cast<char>(v));
}

inline void put_u32_le(std::vector<char> *out, uint32_t v) {
    char buf[4];
    buf[0] = static_cast<char>(v & 0xff);
    buf[1] = static_cast<char>((v >> 8) & 0xff);
    buf[2] = static_cast<char>((v >> 16) & 0xff);
    buf[3] = static_cast<char>((v >> 24) & 0xff);
    out->insert(out->end(), buf, buf + 4);
}

inline void put_u64_le(std::vector<char> *out, uint64_t v) {
    char buf[8];
    for (int i = 0; i < 8; ++i) {
        buf[i] = static_cast<char>((v >> (8 * i)) & 0xff);
    }
    out->insert(out->end(), buf, buf + 8);
}

inline bool get_u8(const std::vector<char> &data, size_t *offset, uint8_t *out) {
    if (*offset + 1 > data.size()) return false;
    *out = static_cast<uint8_t>(data[*offset]);
    *offset += 1;
    return true;
}

inline bool get_u32_le(const std::vector<char> &data, size_t *offset, uint32_t *out) {
    if (*offset + 4 > data.size()) return false;
    uint32_t v = 0;
    v |= (static_cast<uint32_t>(static_cast<uint8_t>(data[*offset + 0]))) << 0;
    v |= (static_cast<uint32_t>(static_cast<uint8_t>(data[*offset + 1]))) << 8;
    v |= (static_cast<uint32_t>(static_cast<uint8_t>(data[*offset + 2]))) << 16;
    v |= (static_cast<uint32_t>(static_cast<uint8_t>(data[*offset + 3]))) << 24;
    *out = v;
    *offset += 4;
    return true;
}

inline bool get_u64_le(const std::vector<char> &data, size_t *offset, uint64_t *out) {
    if (*offset + 8 > data.size()) return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= (static_cast<uint64_t>(static_cast<uint8_t>(data[*offset + i]))) << (8 * i);
    }
    *out = v;
    *offset += 8;
    return true;
}

inline bool get_uuid(const std::vector<char> &data, size_t *offset, uuid_u *out) {
    if (*offset + uuid_u::kStaticSize > data.size()) return false;
    std::memcpy(out->data(), &data[*offset], uuid_u::kStaticSize);
    *offset += uuid_u::kStaticSize;
    return true;
}

// Append the 16 bytes of a uuid to the buffer in network order
// (the canonical UUID byte order used everywhere in RethinkDB).
inline void put_uuid(std::vector<char> *out, const uuid_u &u) {
    out->insert(out->end(),
                reinterpret_cast<const char *>(u.data()),
                reinterpret_cast<const char *>(u.data()) + uuid_u::kStaticSize);
}

// ── CRC32C (Castagnoli) ─────────────────────────────────────────────
//
// Polynomial 0x1EDC6F41, reflected. Used for record integrity. We
// precompute a 256-entry table at first use; the table itself is a
// plain static array initialised once via std::atomic_flag-ish init.
//
// The Castagnoli polynomial is preferred over IEEE CRC32 because it
// has hardware acceleration on modern x86 (CRC32C) and ARM, and it
// is the same polynomial used by ext4/btrfs for metadata checksums
// — keeping us consistent with filesystem-level integrity.

struct crc32c_table_t {
    uint32_t entry[256];
    crc32c_table_t() {
        constexpr uint32_t poly = 0x82f63b78u;  // reflected Castagnoli
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (poly ^ (c >> 1)) : (c >> 1);
            }
            entry[i] = c;
        }
    }
};

const crc32c_table_t &crc32c_table() {
    static const crc32c_table_t instance;
    return instance;
}

}  // anonymous namespace

uint32_t logical_journal_t::compute_crc32(const char *data, size_t len) {
    const crc32c_table_t &tbl = crc32c_table();
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; ++i) {
        crc = tbl.entry[(crc ^ static_cast<uint8_t>(data[i])) & 0xffu] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffu;
}

// ── Construction / destruction ───────────────────────────────────────

logical_journal_t::logical_journal_t(const serializer_filepath_t &serializer_dir,
                                     const uuid_u &table_id,
                                     const uuid_u &shard_id,
                                     io_backender_t *io_backender,
                                     perfmon_collection_t *perfmon_collection)
    : home_thread_mixin_t(),
      table_id_(table_id),
      shard_id_(shard_id),
      journal_dir_(serializer_dir.permanent_path() + PATH_SEPARATOR + "cdc_journal"
                   + PATH_SEPARATOR + uuid_to_str(table_id)
                   + PATH_SEPARATOR + uuid_to_str(shard_id)),
      data_path_(journal_dir_ + PATH_SEPARATOR + "journal.data"),
      idx_path_(journal_dir_ + PATH_SEPARATOR + "journal.idx"),
      idx_tmp_path_(journal_dir_ + PATH_SEPARATOR + "journal.idx.tmp"),
      io_backender_(io_backender),
      perfmon_collection_(perfmon_collection) {
    guarantee(io_backender != nullptr);
    guarantee(perfmon_collection != nullptr);
    guarantee(!table_id.is_nil() && !table_id.is_unset());
    guarantee(!shard_id.is_nil() && !shard_id.is_unset());
}

logical_journal_t::~logical_journal_t() {
    // No fsync guarantees to make here — the caller is responsible
    // for `checkpoint()` after every commit. The destructor just
    // releases in-memory state.
}

// ── Filesystem helpers ──────────────────────────────────────────────

namespace {

// Create `path` and any missing parents. Equivalent to `mkdir -p`.
// Idempotent: returns success if the directory already exists.
void mkdir_p(const std::string &path) {
    std::string acc;
    acc.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        acc.push_back(path[i]);
        if (path[i] == '/' && !acc.empty()) {
            struct stat st;
            if (::stat(acc.c_str(), &st) == 0) {
                guarantee(S_ISDIR(st.st_mode),
                          "CDC journal path %s collides with non-directory %s",
                          path.c_str(), acc.c_str());
            } else {
                int res = ::mkdir(acc.c_str(), DIR_MODE);
                if (res != 0 && get_errno() != EEXIST) {
                    guarantee_err(res == 0, "mkdir(%s) failed", acc.c_str());
                }
            }
        }
    }
    // Final component (no trailing slash).
    if (!acc.empty()) {
        struct stat st;
        if (::stat(acc.c_str(), &st) == 0) {
            guarantee(S_ISDIR(st.st_mode),
                      "CDC journal path %s collides with non-directory %s",
                      path.c_str(), acc.c_str());
        } else {
            int res = ::mkdir(acc.c_str(), DIR_MODE);
            if (res != 0 && get_errno() != EEXIST) {
                guarantee_err(res == 0, "mkdir(%s) failed", acc.c_str());
            }
        }
    }
}

// Open with retry on EINTR. Returns -1 on failure; sets *err_out.
int open_retry(const char *path, int flags, mode_t mode, int *err_out) {
    int res;
    do {
        res = ::open(path, flags, mode);
    } while (res == -1 && get_errno() == EINTR);
    if (res == -1) {
        *err_out = get_errno();
        return -1;
    }
    return res;
}

// Write all bytes, retrying on EINTR and partial writes.
bool write_all(int fd, const char *buf, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t res = ::write(fd, buf + written, len - written);
        if (res == -1) {
            if (get_errno() == EINTR) continue;
            return false;
        }
        guarantee(res > 0);
        written += static_cast<size_t>(res);
    }
    return true;
}

// Read up to `len` bytes from `fd` into `buf`. Returns the number of
// bytes read (may be < len only at EOF). Returns -1 on error.
ssize_t read_full(int fd, char *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t res = ::read(fd, buf + total, len - total);
        if (res == 0) {
            return static_cast<ssize_t>(total);
        }
        if (res == -1) {
            if (get_errno() == EINTR) continue;
            return -1;
        }
        total += static_cast<size_t>(res);
    }
    return static_cast<ssize_t>(total);
}

bool fsync_retry(int fd) {
    int res;
    do {
        res = ::fsync(fd);
    } while (res == -1 && get_errno() == EINTR);
    return res == 0;
}

}  // anonymous namespace

// ── create() ─────────────────────────────────────────────────────────

void logical_journal_t::create(UNUSED signal_t *interruptor) {
    assert_thread();
    mkdir_p(journal_dir_);

    // Open or create data file. The data file is opened with
    // O_APPEND so that concurrent appends (allowed by POSIX even
    // across processes if file offsets are aligned to <= PIPE_BUF)
    // are at most chunked at record boundaries. Within a single
    // process, we serialize append_records() on the home thread, so
    // O_APPEND is belt-and-braces.
    {
        int err = 0;
        int fd = open_retry(data_path_.c_str(),
                            O_WRONLY | O_APPEND | O_CREAT,
                            FILE_MODE,
                            &err);
        if (fd == -1) {
            crash("logical_journal_t::create: failed to open %s (%s)",
                  data_path_.c_str(), errno_string(err).c_str());
        }
        // Touch the file with one fsync to durably create the entry
        // in the directory before we write any records.
        bool ok = fsync_retry(fd);
        ::close(fd);
        guarantee(ok, "fsync on new journal data file failed");
    }

    // Open or create the index file. If absent, write the magic
    // header. Idempotent: if the file already exists with valid
    // magic, leave it alone.
    {
        struct stat st;
        bool index_exists = (::stat(idx_path_.c_str(), &st) == 0);
        if (index_exists) {
            if (st.st_size > 0 && st.st_size < static_cast<off_t>(INDEX_HEADER_SIZE)) {
                crash("logical_journal_t::create: index file %s is truncated (size=%lld)",
                      idx_path_.c_str(), static_cast<int64_t>(st.st_size));
            }
        } else {
            int err = 0;
            int fd = open_retry(idx_path_.c_str(),
                                O_WRONLY | O_CREAT | O_TRUNC,
                                FILE_MODE,
                                &err);
            if (fd == -1) {
                crash("logical_journal_t::create: failed to create %s (%s)",
                      idx_path_.c_str(), errno_string(err).c_str());
            }
            bool ok = write_all(fd, INDEX_MAGIC, INDEX_HEADER_SIZE)
                && fsync_retry(fd);
            ::close(fd);
            guarantee(ok, "failed to write index header to %s", idx_path_.c_str());
        }
    }

    // Stat the data file once to record the current size; recovery
    // will compare this against the index's claimed size.
    {
        struct stat st;
        if (::stat(data_path_.c_str(), &st) == 0) {
            data_file_size_ = static_cast<uint64_t>(st.st_size);
        } else {
            data_file_size_ = 0;
        }
    }

    created_ = true;
    logINF("CDC journal created: table=%s shard=%s dir=%s",
           uuid_to_str(table_id_).c_str(),
           uuid_to_str(shard_id_).c_str(),
           journal_dir_.c_str());
}

// ── recover() ────────────────────────────────────────────────────────

void logical_journal_t::recover(signal_t *interruptor) {
    assert_thread();
    guarantee(created_, "recover() called before create()");

    // 1. Read the index file. If empty (just the magic), index_ stays empty.
    std::vector<char> idx_bytes = read_entire_file(idx_path_, interruptor);
    if (idx_bytes.size() < INDEX_HEADER_SIZE) {
        crash("logical_journal_t::recover: index file %s is truncated",
              idx_path_.c_str());
    }
    if (std::memcmp(idx_bytes.data(), INDEX_MAGIC, INDEX_HEADER_SIZE) != 0) {
        crash("logical_journal_t::recover: bad magic in %s", idx_path_.c_str());
    }
    if (idx_bytes.size() > INDEX_HEADER_SIZE
        && (idx_bytes.size() - INDEX_HEADER_SIZE) % INDEX_ENTRY_SIZE != 0) {
        logWRN("CDC journal %s: index file has partial trailing entry, truncating",
               idx_path_.c_str());
    }
    index_.clear();
    size_t off = INDEX_HEADER_SIZE;
    while (off + INDEX_ENTRY_SIZE <= idx_bytes.size()) {
        index_entry_t e;
        std::memcpy(&e.first_lsn.value, &idx_bytes[off + 0], 8);
        std::memcpy(&e.last_lsn.value,  &idx_bytes[off + 8], 8);
        std::memcpy(&e.file_offset,     &idx_bytes[off + 16], 8);
        std::memcpy(&e.byte_length,     &idx_bytes[off + 24], 4);
        off += INDEX_ENTRY_SIZE;

        // Sanity: contiguous LSNs across entries.
        if (!index_.empty()) {
            const index_entry_t &prev = index_.back();
            guarantee(prev.last_lsn.value + 1 == e.first_lsn.value,
                      "CDC journal index %s: non-contiguous LSNs "
                      "between entry ending at %" PRIu64 " and entry starting at %" PRIu64,
                      idx_path_.c_str(),
                      prev.last_lsn.value, e.first_lsn.value);
        }
        index_.push_back(e);
    }

    // 2. Read the data file and verify it covers every index entry.
    std::vector<char> data_bytes = read_entire_file(data_path_, interruptor);
    data_file_size_ = data_bytes.size();
    uint64_t max_covered_offset = 0;
    for (const index_entry_t &e : index_) {
        if (e.file_offset + e.byte_length > data_bytes.size()) {
            logWRN("CDC journal %s: data file (size=%zu) does not cover index entry "
                   "at offset=%" PRIu64 " length=%u — truncating index",
                   data_path_.c_str(), data_bytes.size(),
                   e.file_offset, e.byte_length);
            // Discard this and all subsequent entries.
            while (!index_.empty()
                   && index_.back().file_offset + index_.back().byte_length > data_bytes.size()) {
                index_.pop_back();
            }
            break;
        }
        max_covered_offset = std::max(max_covered_offset, e.file_offset + e.byte_length);
    }

    // 3. Truncate the data file if it has trailing bytes beyond the
    //    last index entry. We do this because a crashed process may
    //    have written a partial record that we don't have in the
    //    index (the in-memory index_entry was never flushed because
    //    checkpoint() hadn't run). Such bytes are unrecoverable and
    //    would confuse read_from().
    if (max_covered_offset < data_bytes.size()) {
        logWRN("CDC journal %s: truncating %zu trailing bytes after last index entry "
               "(last_index_offset+len=%" PRIu64 ", file_size=%" PRIu64 ")",
               data_path_.c_str(),
               data_bytes.size() - max_covered_offset,
               max_covered_offset, data_bytes.size());
        truncate_data_file(max_covered_offset, interruptor);
        data_file_size_ = max_covered_offset;
    }

    // 4. Set high-water LSN.
    if (index_.empty()) {
        high_water_lsn_val_.store(0);
    } else {
        high_water_lsn_val_.store(index_.back().last_lsn.value);
    }

    recovered_ = true;
    logINF("CDC journal recovered: table=%s shard=%s records=%zu high_water_lsn=%" PRIu64,
           uuid_to_str(table_id_).c_str(),
           uuid_to_str(shard_id_).c_str(),
           index_.size(),
           high_water_lsn_val_.load());
}

// ── append_records() ─────────────────────────────────────────────────

ql::log_sequence_number_t logical_journal_t::append_records(
        const std::vector<ql::change_record_t> &records,
        UNUSED file_account_t *io_account,
        UNUSED signal_t *interruptor) {
    assert_thread();
    guarantee(recovered_, "append_records() called before recover()");
    if (records.empty()) {
        // Empty batch — caller still wants a starting LSN; allocate
        // it without writing anything. We do not advance the
        // high-water mark (no bytes were written), matching
        // allocate_lsn().
        return ql::log_sequence_number_t{ high_water_lsn_val_.load() + 1 };
    }

    // 1. Serialize each record into a single buffer with framing.
    std::vector<char> framed;
    framed.reserve(records.size() * 128);
    for (size_t i = 0; i < records.size(); ++i) {
        std::vector<char> body;
        serialize_record(records[i], &body);

        record_header_t hdr;
        hdr.format_version = RECORD_FORMAT_VERSION;
        hdr.total_length = static_cast<uint32_t>(sizeof(record_header_t) + body.size());
        hdr.crc32 = compute_crc32(body.data(), body.size());

        // Pack header in little-endian.
        char hdr_buf[sizeof(record_header_t)];
        hdr_buf[0] = static_cast<char>(hdr.format_version);
        std::memcpy(hdr_buf + 1, &hdr.total_length, 4);
        std::memcpy(hdr_buf + 5, &hdr.crc32, 4);

        framed.insert(framed.end(), hdr_buf, hdr_buf + sizeof(record_header_t));
        framed.insert(framed.end(), body.begin(), body.end());
    }

    // 2. Assign LSNs. The first allocated LSN is the one *after* the
    //    current high-water mark.
    uint64_t base = high_water_lsn_val_.load() + 1;
    ql::log_sequence_number_t first{ base };
    guarantee(base + records.size() > base,
              "CDC journal LSN counter overflow on append of %zu records",
              records.size());

    // 3. Append to the data file. We use O_APPEND so that POSIX
    //    guarantees atomicity of writes up to PIPE_BUF; for records
    //    larger than PIPE_BUF we still serialize on the home thread
    //    which is the actual mutual exclusion.
    {
        int err = 0;
        int fd = open_retry(data_path_.c_str(),
                            O_WRONLY | O_APPEND,
                            FILE_MODE,
                            &err);
        if (fd == -1) {
            crash("logical_journal_t::append_records: open(%s) failed (%s)",
                  data_path_.c_str(), errno_string(err).c_str());
        }

        bool ok = write_all(fd, framed.data(), framed.size())
            && fsync_retry(fd);
        int close_err = ::close(fd);
        guarantee(ok, "append_records: write/fsync on %s failed", data_path_.c_str());
        guarantee(close_err == 0, "close on journal data file failed");
    }

    // 4. Walk the framed buffer and add an index_entry per record.
    size_t pos = 0;
    uint64_t file_offset = data_file_size_;
    for (size_t i = 0; i < records.size(); ++i) {
        if (pos + sizeof(record_header_t) > framed.size()) {
            crash("append_records: framed buffer shorter than expected at record %zu",
                  i);
        }
        record_header_t hdr;
        std::memcpy(&hdr, &framed[pos], sizeof(record_header_t));
        if (hdr.format_version != RECORD_FORMAT_VERSION) {
            crash("append_records: unknown record format_version=%u",
                  static_cast<unsigned>(hdr.format_version));
        }
        uint32_t total = hdr.total_length;
        guarantee(total >= sizeof(record_header_t),
                  "append_records: malformed record header total_length=%u", total);
        index_entry_t e;
        e.first_lsn.value = base + i;
        e.last_lsn.value  = base + i;
        e.file_offset = file_offset;
        e.byte_length = total;
        index_.push_back(e);

        pos += total;
        file_offset += total;
    }

    data_file_size_ += framed.size();
    high_water_lsn_val_.store(base + records.size() - 1);

    return first;
}

// ── allocate_lsn() ───────────────────────────────────────────────────

ql::log_sequence_number_t logical_journal_t::allocate_lsn() {
    // Note: this does NOT advance the high-water mark, since no
    // bytes have been written. Callers that want a barrier LSN
    // should follow up with a checkpoint and an empty append_records
    // batch — or a future CDC-06 helper that combines the two.
    return ql::log_sequence_number_t{ high_water_lsn_val_.load() + 1 };
}

// ── high_water_lsn() ─────────────────────────────────────────────────

ql::log_sequence_number_t logical_journal_t::high_water_lsn() const {
    return ql::log_sequence_number_t{ high_water_lsn_val_.load() };
}

// ── high_water_rollback() ─────────────────────────────────────────────

void logical_journal_t::high_water_rollback(UNUSED signal_t *interruptor) {
    assert_thread();
    // Roll back to the last *checkpointed* index entry. We do this
    // by scanning the in-memory index backwards until we find an
    // entry whose offset+length matches the start of the uncommitted
    // range. In practice CDC-04 consumers call checkpoint() after
    // every successful commit, so the uncommitted range is simply
    // the trailing contiguous block of entries with file_offset >=
    // last_checkpoint_offset.
    //
    // Simpler approach for CDC-04: drop all entries since the last
    // checkpoint was called. We track this by snapshotting the index
    // size at every checkpoint() call.
    if (index_.empty()) {
        high_water_lsn_val_.store(0);
        data_file_size_ = 0;
        return;
    }
    // For CDC-04 we always roll back to the last entry that was
    // included in the most recent checkpoint. checkpointed_size_
    // records that boundary.
    if (checkpointed_size_ >= index_.size()) {
        // Nothing to roll back (entire in-memory index was already
        // checkpointed).
        return;
    }
    uint64_t rollback_to_offset = index_[checkpointed_size_ - 1].file_offset
                                + index_[checkpointed_size_ - 1].byte_length;
    truncate_data_file(rollback_to_offset, interruptor);
    index_.resize(checkpointed_size_);
    data_file_size_ = rollback_to_offset;
    if (index_.empty()) {
        high_water_lsn_val_.store(0);
    } else {
        high_water_lsn_val_.store(index_.back().last_lsn.value);
    }
}

// ── read_from() ──────────────────────────────────────────────────────

std::vector<ql::change_record_t> logical_journal_t::read_from(
        ql::log_sequence_number_t since_lsn,
        UNUSED signal_t *interruptor) const {
    assert_thread();
    guarantee(recovered_, "read_from() called before recover()");

    std::vector<ql::change_record_t> out;
    if (index_.empty()) return out;
    if (since_lsn.value >= high_water_lsn_val_.load()) return out;

    // Find the first index entry whose last_lsn > since_lsn.
    auto it = std::upper_bound(
        index_.begin(), index_.end(), since_lsn.value,
        [](uint64_t lsn_value, const index_entry_t &e) {
            return lsn_value < e.first_lsn.value;
        });
    if (it == index_.end()) return out;

    // Read from `it` to end.
    std::vector<char> data_bytes = read_entire_file(data_path_, interruptor);
    out.reserve(index_.end() - it);
    for (; it != index_.end(); ++it) {
        if (it->file_offset + it->byte_length > data_bytes.size()) {
            logWRN("read_from: index entry at offset=%" PRIu64 " len=%u exceeds "
                   "data file size %zu — stopping early",
                   it->file_offset, it->byte_length, data_bytes.size());
            break;
        }
        size_t off = it->file_offset;
        ql::change_record_t rec;
        if (!deserialize_record(data_bytes, &off, &rec)) {
            logWRN("read_from: failed to deserialize record at offset=%" PRIu64,
                   it->file_offset);
            break;
        }
        out.push_back(std::move(rec));
    }
    return out;
}

// ── checkpoint() ─────────────────────────────────────────────────────

void logical_journal_t::checkpoint(UNUSED file_account_t *io_account,
                                   UNUSED signal_t *interruptor) {
    assert_thread();
    guarantee(recovered_, "checkpoint() called before recover()");

    // 1. Build the index bytes: magic + entries.
    std::vector<char> buf;
    buf.reserve(INDEX_HEADER_SIZE + index_.size() * INDEX_ENTRY_SIZE);
    buf.insert(buf.end(), INDEX_MAGIC, INDEX_MAGIC + INDEX_HEADER_SIZE);
    for (const index_entry_t &e : index_) {
        char entry_buf[INDEX_ENTRY_SIZE];
        std::memcpy(entry_buf + 0,  &e.first_lsn.value, 8);
        std::memcpy(entry_buf + 8,  &e.last_lsn.value,  8);
        std::memcpy(entry_buf + 16, &e.file_offset,     8);
        std::memcpy(entry_buf + 24, &e.byte_length,     4);
        buf.insert(buf.end(), entry_buf, entry_buf + INDEX_ENTRY_SIZE);
    }

    // 2. Write to a temporary file, fsync, then atomic-rename onto
    //    the final index path. POSIX guarantees rename(2) is atomic
    //    when src and dst are on the same filesystem, which is true
    //    because idx_tmp_path_ lives next to idx_path_.
    {
        int err = 0;
        int fd = open_retry(idx_tmp_path_.c_str(),
                            O_WRONLY | O_CREAT | O_TRUNC,
                            FILE_MODE,
                            &err);
        if (fd == -1) {
            crash("checkpoint: open(%s) failed (%s)",
                  idx_tmp_path_.c_str(), errno_string(err).c_str());
        }
        bool ok = write_all(fd, buf.data(), buf.size()) && fsync_retry(fd);
        int close_err = ::close(fd);
        guarantee(ok, "checkpoint: write/fsync to %s failed", idx_tmp_path_.c_str());
        guarantee(close_err == 0, "checkpoint: close on tmp index failed");
    }
    if (::rename(idx_tmp_path_.c_str(), idx_path_.c_str()) != 0) {
        int err = get_errno();
        crash("checkpoint: rename(%s -> %s) failed (%s)",
              idx_tmp_path_.c_str(), idx_path_.c_str(), errno_string(err).c_str());
    }
    // Best-effort fsync of parent directory so the rename is durable.
    {
        std::string parent = journal_dir_;
        int dfd = ::open(parent.c_str(), O_RDONLY);
        if (dfd >= 0) {
            fsync_retry(dfd);
            ::close(dfd);
        }
    }

    checkpointed_size_ = index_.size();
}

// ── Observability ────────────────────────────────────────────────────

uint64_t logical_journal_t::retained_bytes() const {
    return data_file_size_;
}

uint64_t logical_journal_t::record_count() const {
    return index_.size();
}

// ── Serialization helpers ────────────────────────────────────────────

void logical_journal_t::serialize_record(const ql::change_record_t &rec,
                                         std::vector<char> *out) const {
    // format_version(1) | table_id(16) | shard_id(16) | lsn(8) |
    // source_cluster_id(16) | operation(1) | commit_timestamp_us(8) |
    // before_image_size(4) | <bytes> | after_image_size(4) | <bytes>
    put_u8(out, RECORD_FORMAT_VERSION);
    put_uuid(out, rec.event_id.table_id);
    put_uuid(out, rec.event_id.shard_id);
    put_u64_le(out, rec.event_id.lsn.value);
    put_uuid(out, rec.event_id.source_cluster_id);
    put_u8(out, static_cast<uint8_t>(rec.op));
    put_u64_le(out, rec.commit_timestamp);
    put_u32_le(out, static_cast<uint32_t>(rec.before_image.size()));
    if (!rec.before_image.empty()) {
        out->insert(out->end(), rec.before_image.begin(), rec.before_image.end());
    }
    put_u32_le(out, static_cast<uint32_t>(rec.after_image.size()));
    if (!rec.after_image.empty()) {
        out->insert(out->end(), rec.after_image.begin(), rec.after_image.end());
    }
}

bool logical_journal_t::deserialize_record(const std::vector<char> &data,
                                           size_t *offset,
                                           ql::change_record_t *out) const {
    size_t off = *offset;

    record_header_t hdr;
    if (off + sizeof(record_header_t) > data.size()) return false;
    std::memcpy(&hdr, &data[off], sizeof(record_header_t));
    if (hdr.format_version != RECORD_FORMAT_VERSION) return false;
    off += sizeof(record_header_t);

    uint32_t body_len = hdr.total_length - sizeof(record_header_t);
    if (off + body_len > data.size()) return false;

    uint32_t actual_crc = compute_crc32(&data[off], body_len);
    if (actual_crc != hdr.crc32) {
        logWRN("logical_journal_t::deserialize_record: CRC32C mismatch "
               "(stored=0x%08x computed=0x%08x)",
               hdr.crc32, actual_crc);
        return false;
    }

    uint8_t fmt = 0;
    if (!get_u8(data, &off, &fmt)) return false;
    if (fmt != RECORD_FORMAT_VERSION) return false;
    if (!get_uuid(data, &off, &out->event_id.table_id)) return false;
    if (!get_uuid(data, &off, &out->event_id.shard_id)) return false;
    if (!get_u64_le(data, &off, &out->event_id.lsn.value)) return false;
    if (!get_uuid(data, &off, &out->event_id.source_cluster_id)) return false;

    uint8_t op_byte = 0;
    if (!get_u8(data, &off, &op_byte)) return false;
    if (op_byte > static_cast<uint8_t>(ql::change_operation_t::REPLACE)) {
        return false;
    }
    out->op = static_cast<ql::change_operation_t>(op_byte);

    uint64_t ts = 0;
    if (!get_u64_le(data, &off, &ts)) return false;
    out->commit_timestamp = ts;

    uint32_t before_size = 0;
    if (!get_u32_le(data, &off, &before_size)) return false;
    if (off + before_size > data.size()) return false;
    out->before_image.assign(data.begin() + off, data.begin() + off + before_size);
    off += before_size;

    uint32_t after_size = 0;
    if (!get_u32_le(data, &off, &after_size)) return false;
    if (off + after_size > data.size()) return false;
    out->after_image.assign(data.begin() + off, data.begin() + off + after_size);
    off += after_size;

    *offset = off;
    return true;
}

// ── read_entire_file / truncate_data_file ────────────────────────────

std::vector<char> logical_journal_t::read_entire_file(
        const std::string &path,
        UNUSED signal_t *interruptor) const {
    int err = 0;
    int fd = open_retry(path.c_str(), O_RDONLY, 0, &err);
    if (fd == -1) {
        if (err == ENOENT) {
            // An empty journal is legal; callers distinguish via
            // index_.empty().
            return std::vector<char>();
        }
        crash("read_entire_file(%s): open failed (%s)",
              path.c_str(), errno_string(err).c_str());
    }

    struct stat st;
    if (::fstat(fd, &st) != 0) {
        int e = get_errno();
        ::close(fd);
        crash("read_entire_file(%s): fstat failed (%s)",
              path.c_str(), errno_string(e).c_str());
    }
    size_t size = static_cast<size_t>(st.st_size);
    std::vector<char> out;
    out.resize(size);
    if (size > 0) {
        ssize_t got = read_full(fd, out.data(), size);
        if (got < 0 || static_cast<size_t>(got) != size) {
            int e = (got < 0) ? get_errno() : 0;
            ::close(fd);
            crash("read_entire_file(%s): short read (got %zd of %zu, errno=%d)",
                  path.c_str(), got, size, e);
        }
    }
    ::close(fd);
    return out;
}

void logical_journal_t::truncate_data_file(uint64_t byte_offset,
                                           UNUSED signal_t *interruptor) {
    int err = 0;
    int fd = open_retry(data_path_.c_str(), O_WRONLY, FILE_MODE, &err);
    if (fd == -1) {
        crash("truncate_data_file: open(%s) failed (%s)",
              data_path_.c_str(), errno_string(err).c_str());
    }
    if (::ftruncate(fd, static_cast<off_t>(byte_offset)) != 0) {
        int e = get_errno();
        ::close(fd);
        crash("truncate_data_file: ftruncate(%s, %" PRIu64 ") failed (%s)",
              data_path_.c_str(), byte_offset, errno_string(e).c_str());
    }
    bool ok = fsync_retry(fd);
    ::close(fd);
    guarantee(ok, "truncate_data_file: fsync failed");
}
