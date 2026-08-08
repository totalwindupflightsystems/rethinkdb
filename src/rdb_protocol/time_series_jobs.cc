// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/time_series_jobs.hpp"  // NOLINT(build/include_order)

#include <algorithm>
#include <cstdint>

#include "arch/runtime/coroutines.hpp"
#include "arch/timing.hpp"
#include "btree/operations.hpp"
#include "btree/time_series_ops.hpp"
#include "config/args.hpp"
#include "concurrency/cond_var.hpp"
#include "containers/uuid.hpp"
#include "errors.hpp"
#include "logger.hpp"
#include "rdb_protocol/btree.hpp"
#include "rdb_protocol/configured_limits.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/env.hpp"
#include "rdb_protocol/erase_range.hpp"
#include "rdb_protocol/func.hpp"
#include "rdb_protocol/protocol.hpp"
#include "rdb_protocol/store.hpp"
#include "rdb_protocol/time_series_errors.hpp"
#include "time.hpp"
#include "utils.hpp"

/* Wake cadence when no time-series config has been seen yet (and for plain
 * tables): the loop still must notice a config appearing, but must not poll
 * hot. */
static constexpr int64_t TIME_SERIES_JOBS_DEFAULT_WAKE_MS = 60000;

/* Minimum and maximum wake cadence once a config is known: retention ticks
 * every chunk_interval seconds, so the loop re-checks at most that often
 * (and at least every 60 s so config changes and compaction are noticed). */
static constexpr uint64_t TIME_SERIES_JOBS_MIN_WAKE_SECONDS = 1;
static constexpr uint64_t TIME_SERIES_JOBS_MAX_WAKE_SECONDS = 60;

/* Compaction cadence and per-chunk age threshold (spec §6.4: "chunk sealed
 * + 1 hour old"). */
static constexpr uint64_t TIME_SERIES_COMPACTION_INTERVAL_SECONDS = 3600;
static constexpr uint64_t TIME_SERIES_COMPACTION_MIN_AGE_SECONDS = 3600;

time_series_jobs_t::time_series_jobs_t(store_t *store)
    : store_(store),
      last_retention_us_(0),
      last_compaction_us_(0),
      /* No config seen yet: re-check at the default wake cadence so a
       * freshly-created retention table's config is discovered quickly.
       * The first pass itself runs immediately (last_retention_us_ == 0). */
      last_chunk_interval_seconds_(TIME_SERIES_JOBS_DEFAULT_WAKE_MS / 1000),
      last_downsample_us_(0),
      last_downsample_interval_seconds_(TIME_SERIES_JOBS_DEFAULT_WAKE_MS / 1000),
      pm_retention_runs(get_num_threads()),
      pm_retention_chunks_expired(get_num_threads()),
      pm_retention_rows_freed(get_num_threads()),
      pm_compaction_runs(get_num_threads()),
      pm_compaction_chunks_rewritten(get_num_threads()),
      pm_downsample_runs(get_num_threads()),
      pm_downsample_chunks_merged(get_num_threads()),
      pm_downsample_rows_written(get_num_threads()),
      pm_corrupt_events(get_num_threads()),
      pm_memberships(
          &store_->perfmon_collection,
          &pm_retention_runs, "time_series_retention_runs",
          &pm_retention_chunks_expired, "time_series_retention_chunks_expired",
          &pm_retention_rows_freed, "time_series_retention_rows_freed",
          &pm_compaction_runs, "time_series_compaction_runs",
          &pm_compaction_chunks_rewritten,
          "time_series_compaction_chunks_rewritten",
          &pm_downsample_runs, "time_series_downsample_runs",
          &pm_downsample_chunks_merged, "time_series_downsample_chunks_merged",
          &pm_downsample_rows_written, "time_series_downsample_rows_written",
          &pm_corrupt_events, "time_series_corrupt_events") {
}

time_series_jobs_t::~time_series_jobs_t() {
    /* The drainer lock held by the coroutine guarantees it has exited before
     * the store tears down; nothing to join here. */
}

void time_series_jobs_t::start(auto_drainer_t *drainer) {
    coro_t::spawn_sometime(
        std::bind(&time_series_jobs_t::run, this, drainer->lock()));
}

void time_series_jobs_t::run(auto_drainer_t::lock_t keepalive) THROWS_NOTHING {
    /* Low priority: live queries (and their reads) always run ahead of the
     * maintenance jobs (§9.3). */
    with_priority_t p(CORO_PRIORITY_TIME_SERIES_JOBS);
    try {
        for (;;) {
            const uint64_t now_us = current_microtime();

            /* A zero last-run stamp means "never ran": the first pass is due
             * immediately so a freshly-created retention table gets its
             * first tick without waiting a whole interval. */
            if (last_retention_us_ == 0
                    || now_us - last_retention_us_
                        >= last_chunk_interval_seconds_ * 1000000ULL) {
                run_retention_pass(keepalive, now_us);
                last_retention_us_ = current_microtime();
            }
            if (last_compaction_us_ == 0
                    || now_us - last_compaction_us_
                        >= TIME_SERIES_COMPACTION_INTERVAL_SECONDS
                            * 1000000ULL) {
                run_compaction_pass(keepalive, now_us);
                last_compaction_us_ = current_microtime();
            }
            if (last_downsample_us_ == 0
                    || now_us - last_downsample_us_
                        >= last_downsample_interval_seconds_ * 1000000ULL) {
                run_downsample_pass(keepalive, now_us);
                last_downsample_us_ = current_microtime();
            }

            const uint64_t wake_s = std::min<uint64_t>(
                std::max<uint64_t>(last_chunk_interval_seconds_,
                                   TIME_SERIES_JOBS_MIN_WAKE_SECONDS),
                TIME_SERIES_JOBS_MAX_WAKE_SECONDS);
            nap(static_cast<int64_t>(wake_s * 1000),
                keepalive.get_drain_signal());
        }
    } catch (const interrupted_exc_t &) {
        /* Store is being destroyed; exit cleanly. */
    }
}

bool time_series_jobs_t::run_retention_pass(
        auto_drainer_t::lock_t keepalive, uint64_t now_us) {
    rdb_live_deletion_context_t deletion_context;
    bool expired_any = false;

    for (;;) {
        /* Serialize against chunked writes: the write path holds
         * time_series_mutex across its catalog load → route → save cycle,
         * and retention must not interleave a catalog load/save with it.
         * The mutex is re-acquired per chunk so a long retention pass never
         * starves live writes. */
        scoped_ptr_t<new_mutex_in_line_t> ts_acq;
        ts_acq.init(new new_mutex_in_line_t(&store_->time_series_mutex));
        ts_acq->acq_signal()->wait_lazily_unordered();

        write_token_t token;
        store_->new_write_token(&token);
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        store_->acquire_superblock_for_write(
            4 + static_cast<int>(TIME_SERIES_RETENTION_ERASE_BATCH),
            write_durability_t::SOFT, &token, &txn, &superblock,
            keepalive.get_drain_signal());

        time_series_catalog_t catalog =
            time_series_ops_t::load_catalog(superblock.get());
        if (catalog.config.enabled) {
            last_chunk_interval_seconds_ = catalog.config.chunk_interval_seconds;
        }
        if (!catalog.config.enabled
                || catalog.config.retention_seconds == 0) {
            /* Not a retention table (or retention disabled): nothing to
             * do. Schedule the next check at the default wake cadence so
             * a freshly-created table's config — which only appears with
             * the first write — is discovered quickly. */
            last_chunk_interval_seconds_ =
                TIME_SERIES_JOBS_DEFAULT_WAKE_MS / 1000;
            superblock.reset();
            txn->commit();
            return expired_any;
        }

        std::vector<size_t> expired = time_series_ops_t::expired_chunks(
            catalog.chunk_index, now_us, catalog.config.retention_seconds);
        if (expired.empty()) {
            superblock.reset();
            txn->commit();
            return expired_any;
        }

        const size_t chunk_idx = expired[0];
        if (time_series_ops_t::chunk_is_corrupt(
                catalog.chunk_index.chunks[chunk_idx])) {
            handle_corrupt_chunk("retention");
            superblock.reset();
            txn->commit();
            return expired_any;
        }

        /* Once a chunk's expiry starts it must complete: a partially erased
         * tree committed with the index still pointing at it would corrupt
         * the table. The erase phase is therefore non-interruptible (same
         * precedent as store_t::reset_data()); the txn acquisition above is
         * still interruptible, so store teardown is only delayed by one
         * bounded chunk-sized unit. */
        cond_t non_interruptor;

        buf_lock_t sindex_block(superblock->expose_buf(),
                                superblock->get_sindex_block_id(),
                                access_t::write);
        std::vector<rdb_modification_report_t> mod_reports;
        uint64_t rows_freed = 0;
        try {
            rows_freed = time_series_ops_t::expire_chunk(
                store_->btree.get(), superblock.get(), &catalog, chunk_idx,
                &deletion_context, &non_interruptor, &mod_reports);
        } catch (const ql::base_exc_t &e) {
            /* Corrupt chunk index: expire_chunk validates BEFORE modifying
             * anything, so this transaction is untouched — commit it (a
             * no-op), mark the table read-only and stop the pass. Valid
             * chunks are never deleted around a corrupt one (§7). */
            handle_corrupt_chunk(strprintf(
                "retention: %s", e.what()));
            superblock.reset();
            txn->commit();
            return expired_any;
        }

        /* Index removal + block frees + catalog save commit together: a
         * crash rolls the whole chunk back and retention resumes from the
         * index on the next tick. */
        time_series_ops_t::save_catalog(superblock.get(), catalog);
        if (!mod_reports.empty()) {
            /* Keep secondary indexes consistent: the erased rows must
             * disappear from sindex B-trees too. Changefeeds are NOT
             * notified — retention is storage-level (§6.5). */
            store_->update_sindexes(txn.get(), &sindex_block, mod_reports,
                                    true /* release_sindex_block */);
        }
        superblock.reset();
        txn->commit();

        ++pm_retention_runs;
        ++pm_retention_chunks_expired;
        pm_retention_rows_freed += static_cast<int64_t>(rows_freed);
        expired_any = true;
        /* Yield between chunks so live queries interleave freely. */
        coro_t::yield();
    }
}

bool time_series_jobs_t::run_compaction_pass(
        auto_drainer_t::lock_t keepalive, uint64_t now_us) {
    rdb_live_deletion_context_t deletion_context;
    bool compacted_any = false;

    for (;;) {
        scoped_ptr_t<new_mutex_in_line_t> ts_acq;
        ts_acq.init(new new_mutex_in_line_t(&store_->time_series_mutex));
        ts_acq->acq_signal()->wait_lazily_unordered();

        write_token_t token;
        store_->new_write_token(&token);
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        store_->acquire_superblock_for_write(
            4 + static_cast<int>(TIME_SERIES_RETENTION_ERASE_BATCH),
            write_durability_t::SOFT, &token, &txn, &superblock,
            keepalive.get_drain_signal());

        time_series_catalog_t catalog =
            time_series_ops_t::load_catalog(superblock.get());
        if (catalog.config.enabled) {
            last_chunk_interval_seconds_ = catalog.config.chunk_interval_seconds;
        }
        if (!catalog.config.enabled) {
            superblock.reset();
            txn->commit();
            return compacted_any;
        }

        std::vector<size_t> compactible = time_series_ops_t::compactible_chunks(
            catalog.chunk_index, now_us,
            TIME_SERIES_COMPACTION_MIN_AGE_SECONDS);
        if (compactible.empty()) {
            superblock.reset();
            txn->commit();
            return compacted_any;
        }

        const size_t chunk_idx = compactible[0];
        if (time_series_ops_t::chunk_is_corrupt(
                catalog.chunk_index.chunks[chunk_idx])) {
            handle_corrupt_chunk("compaction");
            superblock.reset();
            txn->commit();
            return compacted_any;
        }

        /* Same atomicity rule as retention: a rewrite that starts must
         * finish (the old tree is freed only after the new one is fully
         * built), so the work phase is non-interruptible. */
        cond_t non_interruptor;

        uint64_t rows = 0;
        try {
            rows = time_series_ops_t::compact_chunk(
                store_->btree.get(), superblock.get(), &catalog, chunk_idx,
                &deletion_context, &non_interruptor);
        } catch (const ql::base_exc_t &e) {
            /* Validated before modification; the txn is untouched. */
            handle_corrupt_chunk(strprintf(
                "compaction: %s", e.what()));
            superblock.reset();
            txn->commit();
            return compacted_any;
        }

        time_series_ops_t::save_catalog(superblock.get(), catalog);
        superblock.reset();
        txn->commit();

        ++pm_compaction_runs;
        ++pm_compaction_chunks_rewritten;
        compacted_any = true;
        coro_t::yield();
    }
}

bool time_series_jobs_t::run_downsample_pass(
        auto_drainer_t::lock_t keepalive, uint64_t now_us) {
    rdb_live_deletion_context_t deletion_context;
    bool merged_any = false;

    for (;;) {
        /* Serialize against chunked writes (same mutex as retention: the
         * write path holds time_series_mutex across its catalog load →
         * route → save cycle, and the merge must not interleave a catalog
         * load/save with it). Re-acquired per chunk so a long pass never
         * starves live writes. */
        scoped_ptr_t<new_mutex_in_line_t> ts_acq;
        ts_acq.init(new new_mutex_in_line_t(&store_->time_series_mutex));
        ts_acq->acq_signal()->wait_lazily_unordered();

        write_token_t token;
        store_->new_write_token(&token);
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_t> superblock;
        store_->acquire_superblock_for_write(
            4 + static_cast<int>(TIME_SERIES_RETENTION_ERASE_BATCH),
            write_durability_t::SOFT, &token, &txn, &superblock,
            keepalive.get_drain_signal());

        time_series_catalog_t catalog =
            time_series_ops_t::load_catalog(superblock.get());
        if (catalog.config.enabled) {
            last_chunk_interval_seconds_ = catalog.config.chunk_interval_seconds;
            if (!catalog.config.downsample_steps.empty()) {
                /* The downsample cadence is the finest target interval
                 * (spec §6.4: downsampling triggers "every `target_interval`
                 * seconds"). */
                uint64_t finest =
                    catalog.config.downsample_steps[0].target_interval_seconds;
                for (const ql::downsample_step_t &step :
                        catalog.config.downsample_steps) {
                    finest = std::min(finest, step.target_interval_seconds);
                }
                last_downsample_interval_seconds_ = std::max<uint64_t>(finest, 1);
            }
        }
        if (!catalog.config.enabled
                || catalog.config.downsample_steps.empty()) {
            superblock.reset();
            txn->commit();
            return merged_any;
        }

        std::vector<size_t> candidates =
            time_series_ops_t::downsample_candidates(catalog.chunk_index);
        if (candidates.empty()) {
            superblock.reset();
            txn->commit();
            return merged_any;
        }

        const size_t chunk_idx = candidates[0];
        if (time_series_ops_t::chunk_is_corrupt(
                catalog.chunk_index.chunks[chunk_idx])) {
            handle_corrupt_chunk("downsample");
            superblock.reset();
            txn->commit();
            return merged_any;
        }

        /* One bounded unit per transaction: one chunk's rows in memory
         * (§9.3). merge_chunk_into_downsample_steps folds the chunk into
         * ALL steps; the fold plus the watermark commit as one
         * transaction, so a crash rolls the in-flight chunk back entirely
         * (its step writes were never committed) and the next pass resumes
         * from the flag — no duplicate buckets, no double counting. */
        cond_t non_interruptor;

        uint64_t rows_written = 0;
        try {
            rows_written =
                time_series_ops_t::merge_chunk_into_downsample_steps(
                    store_->btree.get(), superblock.get(), &catalog,
                    chunk_idx, &deletion_context, &non_interruptor);
        } catch (const ql::base_exc_t &e) {
            /* Either a corrupt chunk index (validated BEFORE any
             * modification, so this transaction is untouched) or an
             * aggregate evaluation error (a config/data mismatch). Either
             * way the chunk stays unmerged and the pass stops; the next
             * tick retries. */
            if (time_series_ops_t::chunk_is_corrupt(
                    catalog.chunk_index.chunks[chunk_idx])) {
                handle_corrupt_chunk(strprintf(
                    "downsample: %s", e.what()));
            } else {
                logWRN("Time-series downsample merge failed for table %s "
                        "(chunk %zu, %zu rows): %s",
                        uuid_to_str(store_->get_table_id()).c_str(),
                        chunk_idx, catalog.chunk_index.chunks[chunk_idx].row_count,
                        e.what());
            }
            superblock.reset();
            txn->commit();
            return merged_any;
        }

        time_series_ops_t::save_catalog(superblock.get(), catalog);
        superblock.reset();
        txn->commit();

        ++pm_downsample_runs;
        ++pm_downsample_chunks_merged;
        pm_downsample_rows_written += static_cast<int64_t>(rows_written);
        merged_any = true;
        /* Yield between chunks so live queries interleave freely. */
        coro_t::yield();
    }
}

void time_series_jobs_t::handle_corrupt_chunk(const std::string &what) {
    store_->mark_time_series_corrupt();
    ++pm_corrupt_events;
    logERR("Time-series chunk corruption detected on store for table %s; "
           "table marked read-only for time-series writes (error code %s). "
           "Cause: %s",
           uuid_to_str(store_->get_table_id()).c_str(),
           time_series_error::TIME_SERIES_CHUNK_CORRUPT, what.c_str());
}
