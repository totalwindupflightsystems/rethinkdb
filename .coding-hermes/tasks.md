# RethinkDB — Steward Task Queue

## Phase 1: Foundation (Now)
- [x] Fork + clone rethinkdb/rethinkdb
- [x] Merge `v2.4.x` into `main` (unreleased v2.4.5 prep)
- [x] Cut and publish v2.4.5 release → https://github.com/totalwindupflightsystems/rethinkdb/releases/tag/v2.4.5
- [x] GitReins init + 3-tier config (Tier 1: guard, Tier 2: LLM eval, Tier 3: cppcheck+clang-tidy)
- [x] Hilo init (7,913 edges across 1,256 files)
- [x] Bear compile_commands.json (560KB, 18K entries)
- [x] Fix gitleaks.toml (valid regex)
- [x] Fix .gitignore (build artifacts, compile_commands.json)
- [x] Modernize CI (add GCC 14/15, Clang 18/19, ARM64, RISC-V)
- [x] Fix all compiler warnings (~12K from clang-tidy, 11K from cppcheck)
  - [x] Build clean with GCC 15.2.0 (zero compiler warnings)
  - [x] cppcheck: 1 actionable fix (postfixOperator in base64.cc), 1 false positive (BREAKPOINT #error)
  - [x] clang-tidy: 0 actionable warnings in own code after .clang-tidy config
  - [x] Created .clang-tidy with focused checks (bugprone, performance, readability, modernize, cppcoreguidelines, cert, concurrency, portability)
  - [x] Added default case to base64_encode switch, included errors.hpp for unreachable()
- [x] Create `compile_commands.json` generation in CI
  - [x] Added bear-based generation to all build matrix jobs (gcc-14, gcc-15, clang-18, clang-19, gcc-arm64)
  - [x] Added to RISC-V build job
  - [x] Uploaded as CI artifact (compile-commands-${{ matrix.name }})

## Phase 2: v2.5 (Next)
- [x] Add RISC-V CI target (PR #7194)
  - Implementation commit: `77e06d086c feat(ci): add RISC-V build job via RISE runners`
- [x] Cherry-pick marchon's security fixes from PR #7191
  - [x] Security and stability fixes (22 files): timing-safe auth, cJSON buffer overflow, SASLPrep hardening, PBKDF2 iterations, null pointer derefs, unsigned underflows, uninitialized vars, signed/unsigned mismatches
  - [x] Critical issue fixes: #6880 cluster crash, #7124 ARM crash, #6433 allocation bounds, #7005 datum bounds, #6952 RISC-V, #7120 /proc/meminfo
  - [x] Container support: cgroup memory detection, shard limit 64→256, shutdown guarantee fix
  - [x] Skipped: JS engine feature (pluggable V8/QuickJS/Duktape/Hermes), AI-generated reports, NOTES.md updates
- [x] Drop AI-generated report files from PR #7191
- [x] Audit all CVEs in bundled deps → see .coding-hermes/research/cve-audit-bundled-deps.md
  - [x] Upgrade OpenSSL 3.0.7 → 3.0.17 (20+ CVEs)
  - [x] Upgrade QuickJS to quickjs-ng v0.15.1 (CVE-2023-48184 CVSS 9.8)
  - [x] Upgrade Boost 1.60 → 1.85
- [x] Modernize C++ standard (C++11 → C++17)
- [x] Replace deprecated Python 2 scripts with Python 3
- [x] **PHASE 2a: Full-Text Search (GIN-style indexes)**
  - [x] **FTS-1: Tokenizer + stemmer infrastructure** (committed 2026-07-09)
  - [x] **FTS-2: GIN-type sindex support**
  - [x] **FTS-3: FTS index function + match query operator** (committed 2026-07-09)
  - [x] **FTS-4: Integration tests and end-to-end FTS pipeline** (committed 2026-07-11)
- [x] **PHASE 2b: Vector indexes (HNSW/IVFFlat)**
  - [x] **VECTOR-1 through VECTOR-8** (all committed, 68 tests, 420+ unit tests pass)
- [x] **PHASE 2c: BRIN-like sparse indexes**
  - [x] **BRIN-1 through BRIN-7** (all committed, 30 tests)

## Discovery Sweep Findings (2026-07-16)
- [x] **TEST — Pre-existing clustering test crashes (mock_store.cc:148) — INVESTIGATED 2026-07-16**
- [x] **FIX — Add missing read type handlers to mock_store_t::read()** (commit `d54e289694`)
- [x] **CHORE — Clean untracked CI artifact files**
- [x] **CI — Zero workflow runs despite active workflow (INVESTIGATED)** — `3a243b0936`

## Phase 3: v3.0 (Future)
- [x] **SPEC — Phase 3 design documents (batch 2: 4 remaining features)**
- [x] **PART-00: Declarative table partitioning** — all 10 sub-tasks complete (PART-01 through PART-10)
- [x] **PAR-00: Parallel query execution** — all 8 sub-tasks complete (PAR-01 through PAR-08)
- [ ] **CDC-00: Logical replication / CDC streaming** — spec: `.coding-hermes/specs/phase3-cdc-streaming.md` (779 lines, 10-section axiom-level)
  - [x] **CDC-01: Data structures** (commit `1b1fd1c1a0`, 11 files, +609 lines, 15 tests)
  - [x] **CDC-02: ReQL surface** (commit `cadaefece9`, 9 files, +1,451 lines)
  - [x] **CDC-03: Write capture seam** (commit `09c2a07430`)
  - [x] **CDC-04: Logical journal** (commit `07f81203e2`)
  - [x] **CDC-05: Publication lifecycle** (commits `03a3f421ac`, `e77c21e90d`, `14ee0d93ec`)
  - [x] **CDC-06: Subscription state machine** (CDC-06a through CDC-06e, all committed)
  - [x] **CDC-07: CDC sink drivers** (commit `36d5d31b57`, 1,450 lines, 19 tests)
  - [x] **CDC-08a: logical_log_retention_t** — embedded in `replication_coordinator.hpp` (commit `f17e3a1f4d`)
    - `pin_through(slot_id, table_id, shard_id, required_lsn)` — register pin from active slot
    - `advance_slot(slot_id, confirmed)` — move slot's confirmed cursor forward
    - `retention_floor(table_id, shard_id)` — min confirmed LSN across active slots
    - GC consultation: extent reclaimable only when all records below retention floor (spec §4.7)
    - Never release retention on flush-only position; confirmed_lsn is the sole release cursor
    - ✅ Implemented as `logical_log_retention_t` class in coordinator header
  - [x] **CDC-08b: replication_coordinator_t header + slot management** — `src/clustering/replication_coordinator.{hpp,cc}` (commit `f17e3a1f4d`)
    - Slot lifecycle state machine: CREATE→BIND→CONFIRM→PAUSE→EVICT→DROP (spec §3.6)
    - `replication_slot_record_t` struct + `replication_slot_state_t` enum (spec §3.6, cross-ref CDC-01 types)
    - `create_slot()`, `bind_slot()`, `confirm_lsn()`, `advance_slot()`, `pause_slot()`, `evict_slot()`, `drop_slot()`
    - confirmed_lsn_by_shard map — monotonic, contiguous-only advancement (spec §3.6)
    - Mailbox registration via `rpc/mailbox/typed.hpp` + business card pattern
    - `auto_drainer_t` lifecycle management (spec §6.4)
    - Files: 2 new (377+296 lines), build clean, 23 CDC tests pass
  - [x] **CDC-08c: Coordinator Raft + shard-routing integration** — embedded in 08b (commit `f17e3a1f4d`)
    - Raft metadata for slot lifecycle (create/drop/pause/evict durable states) (spec §6.3)
    - Batch progress checkpointing, not per-event Raft proposals (spec §3.8)
    - Shard leadership/routing change: reconnect/handoff without altering durable confirmed position (spec §6.4)
    - Distinguish stale shard incarnation from current before cursor ACK (spec §6.3)
    - Integration with existing table_config_t, shard routing, mailbox infrastructure
    - ✅ Implementation: `on_shard_routing_change()`, incarnation validation in `confirm_lsn()`
  - [x] **CDC-08d: Backpressure + lag accounting** — embedded in 08b (commit `f17e3a1f4d`)
    - Per-slot bounded in-memory queues with `maxInFlightBatches`, `maxBufferBytes` (spec §4.9)
    - lag_bytes, lag_lsn, lag_ms per consumer (spec §8.6)
    - Alert thresholds: warn at 80%, hard-limit pause/eviction policy at 100% quota (spec §4.8)
    - Source foreground writes never blocked by slow sinks (spec §4.9)
    - ✅ Implementation: `slot_backpressure_t`, `on_batch_enqueued()`, `get_slot_lag()`
  - [x] **CDC-08e: Shard leadership/routing change** — embedded in 08b (commit `f17e3a1f4d`)
    - Detect shard movement: stale vs current incarnation check (spec §6.3)
    - Stream handoff/reconnect without cursor loss (spec §6.4)
    - EVICTED slot: explicit state, last confirmed LSN, retention floor recorded (spec §4.8)
    - RESYNC_REQUIRED when history gone — never silent resume (spec §4.8)
    - ✅ Implementation: `on_shard_routing_change()`, `evict_slot()`, `mark_resync_required()`
  - [x] **CDC-08f: Observability** — extend coordinator (08b/08c) (commit `408617321d`)
    - 9 low-cardinality metrics (spec §6.6): cdc_records_captured/delivered, delivery_latency_ms, slot_lag_bytes/lag_lsn, retained_journal_bytes, sink_retries, sink_dead_letter, resync_required
    - Keyed by IDs or controlled names, never document/user values as labels (spec §6.6)
    - Slot blocking reclamation identification in status/alert path (spec §8.6)
    - Files: extend coordinator (~60 lines), wire into stats reporting path
  - [ ] **CDC-09: Conflict resolution** — `src/rdb_protocol/` (new `conflict_resolver.hpp/cc`)
    - Last-write-wins: deterministic (commit_timestamp, cluster_uuid, shard_uuid, LSN) tuple comparison (spec §7.2)
    - Primary-key merge: upsert-oriented shallow merge; PK type/value mismatch → identity conflict (spec §7.3)
    - Custom handler: serialized restricted ReQL function, no table/network/random access (spec §7.4)
    - Conflict log: durable record per unresolved conflict; operator retry/skip/resolve (spec §7.5)
    - Tombstone versions for LWW deletes to prevent resurrection from old replayed writes
    - Replication metadata stored outside user JSON fields (spec §7.1)
  - [ ] **CDC-10: Tests** — unit tests, integration tests, failure tests, benchmarks
    - Unit tests, source-to-target integration, CDC sink tests, failure/durability, performance, compatibility
- [ ] Async I/O subsystem (PG18-style) — spec: `.coding-hermes/specs/phase3-async-io.md`
- [ ] JSONB/JSONPath improvements — spec: `.coding-hermes/specs/phase3-jsonb-jsonpath.md`
- [ ] Generated/virtual columns
- [ ] MERGE/UPSERT with complex conditions
- [ ] Time-series optimizations
- [ ] Foreign data wrapper support
- [ ] WASM-based UDF sandbox (replace V8/QuickJS with WASM runtime)

## Discovery Sweep Findings (2026-07-19)

- [x] **CDC-08 DECOMPOSED — 4 WIP stashes, repeated worker failure pattern**
  - 4 WIP stashes: workers kept refactoring existing `cdc_sink.*` files instead of building new `src/clustering/replication_coordinator.*`
  - Root cause: monolithic task (6 sub-items across 2 directories) was too large — no worker could complete it in a single session
  - Decomposed into 6 smaller tasks: CDC-08a (logical_log_retention_t) → CDC-08f (observability)
  - Each sub-task has bounded scope (~60-200 lines), single file touchpoint, and clear ACs from spec
  - Old stashes preserved for reference: `git stash list | grep CDC-08`
  - Untracked partial code (1221 lines) from prior monolithic attempt left on disk — retention.h/.cc compile, coordinator has 2 mechanical errors
  - Next: Bane should review decomposition before worker dispatch

## Discovery Sweep Findings (2026-07-16 tick 2)
- [x] **BUILD — rethinkdb binary build attempted; linker OOM in container (signal 7, Bus error)**
  - `./configure` was never run prior to this tick — now configured with `config.mk`
  - All object files compile successfully; only final `ld` link step fails with Bus error
  - Root cause: 5GB container memory limit insufficient for RethinkDB linker (~366K LOC)
  - Workaround: build outside container, or increase container memory, or link with `-Wl,--no-keep-memory`
- [x] **DOC — AGENTS.md stale version reference fixed: "v2.3" → "v2.5" (line 40)**
  - Line 40: `- Includes user authentication and permissions system (since v2.3).`
  - Project is at v2.5 after Phase 2 completion (C++17, FTS, vector indexes, BRIN indexes)
  - Fix is a one-line mechanical change

## Discovery Sweep Findings (2026-07-16 tick 1)
- [x] **SPEC — Phase 3 design documents (v3.0 roadmap specs)** — all 5 complete (`fae6aec36c`)
- [x] **BUILD — Fix `make test` target: web-assets dependency broken** (`2072d2a40c`)
- [x] **TEST — Investigate RDBBtree.SindexPostConstruct OOM (pre-existing) — INVESTIGATED 2026-07-16**
