// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_TIME_SERIES_JOBS_HPP_
#define RDB_PROTOCOL_TIME_SERIES_JOBS_HPP_

#include <cstdint>
#include <string>

#include "concurrency/auto_drainer.hpp"
#include "perfmon/perfmon.hpp"

class store_t;

/* Per-store background jobs for time-series tables (PHASE3-TS-4, spec §6.4).
 *
 * Three system-level jobs run on a single low-priority coroutine per store:
 *
 *   - Retention: every `chunk_interval` seconds, deletes chunks whose newest
 *     row is older than `retention` (§5.2). One transaction per chunk, so
 *     each chunk's index removal + block frees commit atomically; a crash
 *     rolls the in-flight chunk back and the next tick resumes from the
 *     (still consistent) chunk index — the index itself is the checkpoint.
 *   - Compaction: at most hourly, rewrites sealed chunks whose data is at
 *     least 1 hour old into fresh, packed B-trees for read efficiency.
 *   - Downsampling: merges sealed raw chunks into the per-step downsample
 *     trees at the finest `target_interval` cadence (§6.4, PHASE3-TS-5).
 *     One transaction per chunk: the fold into every step plus the chunk's
 *     `merged` watermark commit atomically, so a crash rolls the in-flight
 *     chunk back and the next pass resumes from the watermark.
 *
 * Resource limits (§9.3): the loop runs at CORO_PRIORITY_TIME_SERIES_JOBS
 * (behind live queries), wakes at most once per second, holds the
 * superblock / time_series_mutex only for one bounded chunk-sized
 * transaction at a time, and yields between chunks. Memory is bounded by the
 * erase batch (retention) or one chunk's rows (compaction), never by table
 * size.
 *
 * Lifecycle: `start()` spawns the coroutine holding a drainer lock; when the
 * store is destroyed (tableDrop / shutdown) `store_t::~store_t()` drains the
 * drainer, which interrupts and joins the coroutine before any store state
 * is torn down.
 *
 * Corruption (§7): when the chunk index points at structurally invalid
 * chunks (rows claimed with no tree root), the pass aborts BEFORE deleting
 * anything — the store is marked read-only for time-series writes (writes
 * raise TIME_SERIES_CHUNK_CORRUPT) and an alert is logged. Valid chunks are
 * never deleted around a corrupt one.
 */
class time_series_jobs_t : public home_thread_mixin_t {
public:
    explicit time_series_jobs_t(store_t *store);
    ~time_series_jobs_t();

    /* Spawns the job coroutine. Safe to call once, from the store
     * constructor (the store's home thread). */
    void start(auto_drainer_t *drainer);

private:
    /* Main loop: due-checks + nap. Exits on interruption (store teardown). */
    void run(auto_drainer_t::lock_t keepalive) THROWS_NOTHING;

    /* One retention pass: expires every currently-expired chunk, one
     * transaction per chunk. Returns true when at least one chunk was
     * expired; refreshes the last-seen chunk interval from the durable
     * config. */
    bool run_retention_pass(auto_drainer_t::lock_t keepalive,
                            uint64_t now_us);

    /* One compaction pass: rewrites every compactible sealed chunk, one
     * transaction per chunk. Returns true when at least one chunk was
     * rewritten. */
    bool run_compaction_pass(auto_drainer_t::lock_t keepalive,
                             uint64_t now_us);

    /* One downsample pass (PHASE3-TS-5, §6.4): merges the raw rows of every
     * sealed, unmerged chunk into the per-step downsample trees, one
     * transaction per chunk (fold into ALL steps, then set the chunk's
     * `merged` watermark and save the catalog — a crash rolls the in-flight
     * chunk back and the next pass resumes from the watermark). Returns true
     * when at least one chunk was merged. */
    bool run_downsample_pass(auto_drainer_t::lock_t keepalive,
                             uint64_t now_us);

    void handle_corrupt_chunk(const std::string &what);

    store_t *store_;

    /* Last run timestamps (microseconds since epoch) and the chunk interval
     * of the last config seen, which drives the retention cadence. The
     * downsample cadence is the finest target interval seen (spec §6.4:
     * "every `target_interval` seconds"). */
    uint64_t last_retention_us_;
    uint64_t last_compaction_us_;
    uint64_t last_chunk_interval_seconds_;
    uint64_t last_downsample_us_;
    uint64_t last_downsample_interval_seconds_;

    /* ── perfmon (under the store's perfmon_collection) ── */
    perfmon_counter_t pm_retention_runs;
    perfmon_counter_t pm_retention_chunks_expired;
    perfmon_counter_t pm_retention_rows_freed;
    perfmon_counter_t pm_compaction_runs;
    perfmon_counter_t pm_compaction_chunks_rewritten;
    perfmon_counter_t pm_downsample_runs;
    perfmon_counter_t pm_downsample_chunks_merged;
    perfmon_counter_t pm_downsample_rows_written;
    perfmon_counter_t pm_corrupt_events;
    perfmon_multi_membership_t pm_memberships;

    DISABLE_COPYING(time_series_jobs_t);
};

#endif  // RDB_PROTOCOL_TIME_SERIES_JOBS_HPP_
