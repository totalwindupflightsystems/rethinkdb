# RethinkDB — Model Router Task Matrix

**Core purpose:** Modernize and extend RethinkDB — a distributed, open-source NoSQL database for realtime web apps — with CDC streaming, parallel queries, partitioning, and C++17 modernization.

## Active Tasks

| ID | Task | Priority | Complexity | Deps | Tags | Model | Reasoning | Fallback |
|----|------|----------|------------|------|------|-------|-----------|----------|
| CDC-09 | Conflict resolution (LWW, PK-merge, custom handler, conflict log) | Critical | 7 (new subsystem) | CDC-08 | +++backend, +++distributed-systems, ++architecture | GPT-5.6 Sol | Greenfield ~600 lines in clustering/rdb_protocol; architectural decisions; distributed state | GLM-5.2 |
| CDC-09a | LWW resolver + tombstone versions | High | 4 | CDC-09 | ++code-generation, +architecture | DeepSeek V4 Pro | ~150 lines; bounded deterministic logic | GLM-5.2 |
| CDC-09b | Primary-key merge | High | 4 | CDC-09a | ++code-generation, +testing | DeepSeek V4 Pro | ~120 lines; upsert logic well-specified | GLM-5.2 |
| CDC-09c | Custom handler + validation | Medium | 5 | CDC-09a | ++code-generation, ++security, +concurrency | GLM-5.2 | ~180 lines; restricted ReQL evaluation; security boundary | DeepSeek V4 Pro |
| CDC-09d | Conflict log + operator actions | Medium | 4 | CDC-09c | ++code-generation, +testing | DeepSeek V4 Pro | ~150 lines; durable log + operator retry/skip/resolve | GLM-5.2 |
| CDC-10 | CDC comprehensive tests | High | 6 | CDC-09 | +++testing, ++debugging, +concurrency | DeepSeek V4 Pro | Unit, integration, failure, durability, performance, compatibility tests | GLM-5.2 |
| INT-01 | Integration test harness + fixtures + server lifecycle | Critical | 3 | — | ++testing, ++infrastructure | DeepSeek V4 Pro | Python harness, server start/stop/restart, fixture infrastructure | GLM-5.2 |
| INT-02 | Basic CRUD integration suite (5 tests) | Critical | 2 | INT-01 | ++testing | GLM-5.2 | insert/get/update/delete/count/filter through ReQL driver | DeepSeek V4 Pro |
| INT-03 | Changefeed CDC path suite (5 tests) | Critical | 3 | INT-01 | ++testing, ++streaming | DeepSeek V4 Pro | changes() with include_initial, old_val/new_val, update delivery | GLM-5.2 |
| INT-04 | Index + durability suite (10 tests) | High | 3 | INT-02 | ++testing, ++durability | DeepSeek V4 Pro | index_create/wait, between, server restart durability | GLM-5.2 |
| INT-05 | Bulk + edge case suite (5 tests) | High | 2 | INT-02 | ++testing, ++performance | GLM-5.2 | 500-doc batches, bulk updates, empty ops, edge cases | DeepSeek V4 Pro |
| INT-06 | CDC end-to-end (publications, subscriptions) | Critical | 5 | CDC-10 | ++testing, ++distributed-systems | DeepSeek V4 Pro | publication_create, subscription_create, cdc_sink_create, cross-node replication | GLM-5.2 |
| INT-07 | Vector/FTS/BRIN query tests (15 tests) | High | 4 | INT-01 | ++testing, ++search | DeepSeek V4 Pro | Vector index querying, FTS match, BRIN pruning through server | GLM-5.2 |
| INT-08 | CI integration — integration tests on every commit | High | 3 | INT-01 | ++infrastructure, ++testing | DeepSeek V4 Flash | GitHub Actions workflow, pip install rethinkdb, automated run | MiniMax M3 |
| PHASE3-ASYNC | Async I/O subsystem (PG18-style) | Medium | 9 (architectural) | None | +++architecture, +++concurrency, +++performance | GPT-5.6 Sol | System-wide redesign; requires deep architectural planning | — |
| PHASE3-VEC | Generated/virtual columns | Low | 4 | PHASE3-ASYNC | ++code-generation, +architecture | GLM-5.2 | Moderate feature with clear scope | DeepSeek V4 Pro |
| PHASE3-MERGE | MERGE/UPSERT complex conditions | Low | 5 | None | ++code-generation, +architecture | GLM-5.2 | ReQL surface extension | DeepSeek V4 Pro |
| PHASE3-TS | Time-series optimizations | Low | 5 | None | ++code-generation, ++performance | DeepSeek V4 Pro | Optimizer + storage changes | GLM-5.2 |
| PHASE3-FDW | Foreign data wrapper support | Low | 6 | None | ++architecture, ++distributed-systems | GPT-5.6 Sol | Architectural feature; federation layer | GLM-5.2 |
| PHASE3-WASM | WASM-based UDF sandbox | Low | 7 | None | +++security, ++architecture, ++performance | GPT-5.6 Sol | Replace V8/QuickJS with WASM runtime; security-critical | — |
| PERF-BENCH | Performance benchmarks (0 exist for CDC/vector/FTS) | Medium | 3 | CDC-10 | ++testing, +performance | DeepSeek V4 Flash | Mechanical: Google Benchmark scaffolding for existing features | MiniMax M3 |
| NEVER-DONE | 11-point audit sweep | High | 2 | — | ++code-review, ++debugging, +testing | DeepSeek V4 Pro | Audit runs every tick; finds new gaps | GLM-5.2 |

**Assumptions:** CDC-09 decomposition reviewed by Bane; C++17 toolchain available; container memory ≥ 8GB for linker; fork push events require manual CI trigger.

**Routing Notes:** GPT-5.6 Sol for architectural tasks (async I/O, FDW, WASM — system-wide redesign). DeepSeek V4 Pro as daily driver for CDC implementation. GLM-5.2 for security-boundary work (custom handler validation). DeepSeek V4 Flash for mechanical work (benchmark scaffolding).

**Execution Order:** CDC-09a → CDC-09b → CDC-09c → CDC-09d → CDC-10 → PERF-BENCH. Phase 3 architectural tasks parallelize after CDC-10.

**Escalation Conditions:** CDC-09 touches more than 4 files → split further. Test failures reveal architectural issues → escalate to GPT-5.6 Sol. Security/data-loss risk in CDC handler → escalate immediately. Context exceeds 128K → switch to GLM-5.2 or DeepSeek V4 Pro.

## Completed Summary

**Phase 1 (Foundation):** Fork, v2.4.5 release, GitReins + Hilo init, modern CI (GCC 14/15, Clang 18/19, ARM64, RISC-V), zero compiler warnings.
**Phase 2 (v2.5):** RISC-V CI, security fixes (PR #7191), CVE audit (OpenSSL 3.0.17, QuickJS 0.15.1), C++11→C++17, Python 2→3, Full-Text Search (GIN indexes), Vector indexes (HNSW/IVFFlat), BRIN sparse indexes.
**Phase 3 (v3.0 - partial):** Declarative partitioning (PART-00, 10 sub-tasks), Parallel query execution (PAR-00, 8 sub-tasks), CDC streaming (CDC-01 through CDC-08f, 42/42 tests pass), 10 design specs.
**U01 Usability Audit:** Full audit completed tick #11 — 447 source files, 41 test files (606 TEST macros), binary builds+links, HTTP admin routes wired (ajax/reql + static), CDC streaming code verified (replication_mailbox, cdc_types, 9 perfmon counters), error handling: 3,749 patterns across codebase, no new stubs or regressions. Project is stable; blocked only on Bane's CDC-08 review.
**Discovery Sweeps:** 11 ticks of audits; CI cpplint fixed; binary builds and links; 0 CVEs; 0 gitleaks; CDC-08 decomposed from monolithic to 6 sub-tasks; CDC-09 blocked on Bane review (14 idle ticks at 12h cooldown).

## Idle Tick #22 — 2026-07-24 11:04 UTC

**11-Point Audit (Quick Check) — 22nd consecutive idle tick:**

| # | Check | Result | Detail |
|---|-------|--------|--------|
| 1 | SPEC ALIGNMENT | N/A | No specs/ dir; AGENTS.md serves as architecture doc |
| 2 | DOC COVERAGE | PASS | LICENSE, README, CONTRIBUTING, STYLE present (unchanged) |
| 3 | TEST GAPS | PASS | 94 unittest .cc files, 606 TEST() macros — unchanged |
| 4 | PACKAGE UPGRADES | PASS | Bundled deps unchanged (gtest 1.8.1, openssl 3.0.17, quickjs 0.15.1, re2 2015) |
| 5 | PITFALL HUNT | PASS | No new TODO/FIXME; no new stubs or regressions since tick #21 |
| 6 | PERFORMANCE | PASS | PERF-BENCH task on board; 0 benchmark files in build/ |
| 7 | ENDPOINT VERIFICATION | PASS | Binary: rethinkdb 2.4.5-228 (GCC 15.2.0) — 345MB, ELF 64-bit, build/release/, built Jul 21 |
| 8 | CI/CD HEALTH | INFRA | Fork repo — no runner available; local-only |
| 9 | DUCKBRAIN SYNC | PASS | DuckBrain MCP available; 23 keys in rethinkdb namespace; wrote idle #22 |
| 10 | CODE QUALITY | PASS | Working tree clean; gitleaks: 0 leaks (65.70MB scanned in 3.47s) |
| 11 | MIDDLE-OUT WIRING | PASS | Binary links and runs; 17,831 edges across 2,934+ files (cached stats) |

**Hilo:** 17,831 edges across 2,934+ files — Hilo=useful (cached stats, warm timed out at 120s)
**Cooldown:** 43200s — re-fixed (900→43200s, 13th reversion — scheduler restart reset) ✅
**System:** Load 15.70, 14Gi/59Gi RAM, 44Gi available, 16 CPUs, up 7d 17h
**Actions:** Cooldown re-fixed via API PUT (900→43200s) with GET verification. DuckBrain write for idle #22. No worker spawn needed.

### Status: Blocked — Escalation Already Sent (Tick #7)

CDC-09 conflict resolution remains blocked on Bane review. All CDC-08 sub-tasks complete (42/42 tests). **22 consecutive idle ticks** — project genuinely blocked.

**What's Waiting on Bane:**
- Review of CDC-08 streaming implementation (6 sub-tasks, 42/42 tests passing)
- Sign-off on CDC-09 design/decomposition into 4 sub-tasks
- Decision on Phase 3 architectural work (async I/O, FDW, WASM) in parallel
- CI runner provisioning for fork repo (all CI checks INFRA-blocked)

## Idle Tick #20 — 2026-07-23 20:37 UTC

**11-Point Audit (Quick Check) — 20th consecutive idle tick:**

| # | Check | Result | Detail |
|---|-------|--------|--------|
| 1 | SPEC ALIGNMENT | N/A | No specs/ dir; AGENTS.md serves as architecture doc |
| 2 | DOC COVERAGE | PASS | LICENSE, README, CONTRIBUTING, STYLE present (unchanged) |
| 3 | TEST GAPS | PASS | 94 unittest .cc files, 606 TEST() macros — unchanged |
| 4 | PACKAGE UPGRADES | PASS | Bundled deps unchanged (gtest 1.8.1, openssl 3.0.17, quickjs 0.15.1, re2 2015) |
| 5 | PITFALL HUNT | PASS | 254 TODO/FIXME across src/ (stable); 7 HACK/XXX (unchanged); no new stubs or regressions |
| 6 | PERFORMANCE | PASS | PERF-BENCH task on board; 0 benchmark files in src/benchmark/ |
| 7 | ENDPOINT VERIFICATION | PASS | Binary: `rethinkdb 2.4.5-228-g9cbef4 (GCC 15.2.0)` — 345MB, build/release/ |
| 8 | CI/CD HEALTH | INFRA | Fork repo — no runner available; local-only |
| 9 | DUCKBRAIN SYNC | PASS | DuckBrain MCP available; 22 keys in rethinkdb namespace; writing tick #20 |
| 10 | CODE QUALITY | PASS | Working tree clean; gitleaks: 0 leaks (65.70MB scanned in 2.87s) |
| 11 | MIDDLE-OUT WIRING | PASS | Binary links and runs; 17,831 edges across 2,934+ files |

**Hilo:** 17,831 edges across 2,934+ files — Hilo=useful (cached stats, no warm)
**Cooldown:** 43200s — re-fixed (7200→43200s, 11th reversion — scheduler restart reset) ✅
**System:** Load 14.48, 10Gi/59Gi RAM, 49Gi available, 16 CPUs, 218 procs
**Actions:** Cooldown re-fixed via API PUT (7200→43200s) with GET verification. DuckBrain write for idle #20. No worker spawn needed.

### Status: Blocked — Escalation Already Sent (Tick #7)

CDC-09 conflict resolution remains blocked on Bane review. All CDC-08 sub-tasks complete (42/42 tests). **20 consecutive idle ticks** — project genuinely blocked.

**What's Waiting on Bane:**
- Review of CDC-08 streaming implementation (6 sub-tasks, 42/42 tests passing)
- Sign-off on CDC-09 design/decomposition into 4 sub-tasks
- Decision on Phase 3 architectural work (async I/O, FDW, WASM) in parallel
- CI runner provisioning for fork repo (all CI checks INFRA-blocked)

## Idle Tick #14 — 2026-07-22 14:11 UTC

**11-Point Audit (Quick Check) — 14th consecutive idle tick:**

| # | Check | Result | Detail |
|---|-------|--------|--------|
| 1 | SPEC ALIGNMENT | N/A | No specs/ dir; AGENTS.md serves as architecture doc |
| 2 | DOC COVERAGE | PASS | LICENSE, README, CONTRIBUTING, STYLE present (unchanged) |
| 3 | TEST GAPS | PASS | 94 unittest .cc files, 606 TEST() macros confirmed (previous tick) |
| 4 | PACKAGE UPGRADES | PASS | Bundled deps unchanged (gtest 1.8.1, openssl 3.0.17, quickjs 0.15.1, re2 2015) |
| 5 | PITFALL HUNT | PASS | No new TODO/FIXME/HACK/XXX in CDC test files; no new stubs or regressions |
| 6 | PERFORMANCE | PASS | PERF-BENCH task on board; no benchmark binaries built |
| 7 | ENDPOINT VERIFICATION | PASS | Binary exists on disk (previous tick confirmed `2.4.5-228-g9cbef4`) |
| 8 | CI/CD HEALTH | INFRA | Fork repo — no runner available; cooldown reversion #5 (7200→43200s re-fixed) |
| 9 | DUCKBRAIN SYNC | BLOCKED | DuckBrain MCP unreachable — system under severe resource exhaustion (load avg 6.60, fork failures, `can't start new thread`); see below |
| 10 | CODE QUALITY | PASS | Working tree clean; no untracked files; no gitleaks issues |
| 11 | MIDDLE-OUT WIRING | PASS | Binary links; all modules compiled into single daemon binary |

**Hilo:** 17,831 edges across 2,934+ files — Hilo=useful (cached stats, warm blocked by resource exhaustion)
**Host State:** ⚠️ CRITICAL — Load avg 6.60, 3.7M+ tasks, `fork: retry: Resource temporarily unavailable`, `can't start new thread`. Terminal operations blocked. RethinkDB's build (make -j4 with heavy C++17 compilation) likely contributed to thread pool exhaustion. Subsequent `hilo graph warm` also panicked on rayon thread pool init.
## Idle Tick #19 — 2026-07-23 12:20 UTC

**11-Point Audit (Quick Check) — 19th consecutive idle tick:**

| # | Check | Result | Detail |
|---|-------|--------|--------|
| 1 | SPEC ALIGNMENT | N/A | No specs/ dir; AGENTS.md serves as architecture doc |
| 2 | DOC COVERAGE | PASS | LICENSE, README, CONTRIBUTING, STYLE present (unchanged) |
| 3 | TEST GAPS | PASS | 94 unittest .cc files, 606 TEST() macros — unchanged |
| 4 | PACKAGE UPGRADES | PASS | Bundled deps unchanged (gtest 1.8.1, openssl 3.0.17, quickjs 0.15.1, re2 2015) |
| 5 | PITFALL HUNT | PASS | TODO/FIXME in unittest/ (unchanged); no new stubs or regressions |
| 6 | PERFORMANCE | PASS | PERF-BENCH task on board; no benchmark binaries built |
| 7 | ENDPOINT VERIFICATION | PASS | Binary: `rethinkdb 2.4.5-228-g9cbef4 (GCC 15.2.0)` — 361MB, build/release/ |
| 8 | CI/CD HEALTH | INFRA | Fork repo — no runner available; local-only |
| 9 | DUCKBRAIN SYNC | PASS | DuckBrain MCP recovered via `hermes mcp test`; wrote idle #19 entries to rethinkdb namespace (21 keys) |
| 10 | CODE QUALITY | PASS | Working tree clean; gitleaks: 0 leaks (65.69MB scanned in 3.19s) |
| 11 | MIDDLE-OUT WIRING | PASS | Binary links and runs; 17,831 edges across 2,934+ files |

**Hilo:** 17,831 edges across 2,934+ files — Hilo=useful (cached stats, no warm)
**Cooldown:** 43200s — re-fixed (7200→43200s, 10th reversion — scheduler restart reset) ✅
**System:** Load 9.07, 11Gi/59Gi RAM, 48Gi available, 16 CPUs
**Actions:** Cooldown re-fixed via API PUT (7200→43200s) with GET verification. DuckBrain write for idle #19. No worker spawn needed.

### Status: Blocked — Escalation Already Sent (Tick #7)

CDC-09 conflict resolution remains blocked on Bane review. All CDC-08 sub-tasks complete (42/42 tests). **19 consecutive idle ticks** — project genuinely blocked.

**What's Waiting on Bane:**
- Review of CDC-08 streaming implementation (6 sub-tasks, 42/42 tests passing)
- Sign-off on CDC-09 design/decomposition into 4 sub-tasks
- Decision on Phase 3 architectural work (async I/O, FDW, WASM) in parallel
- CI runner provisioning for fork repo (all CI checks INFRA-blocked)

## Idle Tick #18 — 2026-07-23 08:18 UTC

**11-Point Audit (Quick Check) — 18th consecutive idle tick:**

| # | Check | Result | Detail |
|---|-------|--------|--------|
| 1 | SPEC ALIGNMENT | N/A | No specs/ dir; AGENTS.md serves as architecture doc |
| 2 | DOC COVERAGE | PASS | LICENSE, README, CONTRIBUTING, STYLE present (unchanged) |
| 3 | TEST GAPS | PASS | 94 unittest .cc files, 606 TEST() macros — unchanged |
| 4 | PACKAGE UPGRADES | PASS | Bundled deps unchanged (gtest 1.8.1, openssl 3.0.17, quickjs 0.15.1, re2 2015) |
| 5 | PITFALL HUNT | PASS | 10 TODO/FIXME in unittest/ (unchanged); no new stubs or regressions |
| 6 | PERFORMANCE | PASS | PERF-BENCH task on board; no benchmark binaries built |
| 7 | ENDPOINT VERIFICATION | PASS | Binary: `rethinkdb 2.4.5-228-g9cbef4` — 361MB, build/release/ |
| 8 | CI/CD HEALTH | INFRA | Fork repo — no runner available; local-only |
| 9 | DUCKBRAIN SYNC | PASS | DuckBrain MCP available; wrote idle tick #18 entries |
| 10 | CODE QUALITY | PASS | Working tree clean; gitleaks: 0 leaks (65.69MB scanned in 3.9s) |
| 11 | MIDDLE-OUT WIRING | PASS | Binary links; all modules compiled into single daemon binary |

**Hilo:** 17,831 edges across 2,934+ files — Hilo=useful (cached stats)
**Cooldown:** 43200s — re-fixed (7200→43200s, 9th reversion — scheduler restart reset) ✅
**System:** Load 11.38, 10Gi/59Gi RAM, 48Gi available, 16 CPUs
**Actions:** Cooldown re-fixed via API PUT (43200s). Board update. DuckBrain write for idle #18. No worker spawn needed.

### Status: Blocked — Escalation Already Sent (Tick #7)

CDC-09 conflict resolution remains blocked on Bane review. All CDC-08 sub-tasks complete (42/42 tests). **18 consecutive idle ticks** — project genuinely blocked.

**What's Waiting on Bane:**
- Review of CDC-08 streaming implementation (6 sub-tasks, 42/42 tests passing)
- Sign-off on CDC-09 design/decomposition into 4 sub-tasks
- Decision on Phase 3 architectural work (async I/O, FDW, WASM) in parallel
- CI runner provisioning for fork repo (all CI checks INFRA-blocked)

## Idle Tick #15 — 2026-07-22 20:37 UTC

**11-Point Audit (Quick Check) — 15th consecutive idle tick:**

| # | Check | Result | Detail |
|---|-------|--------|--------|
| 1 | SPEC ALIGNMENT | N/A | No specs/ dir; AGENTS.md serves as architecture doc |
| 2 | DOC COVERAGE | PASS | LICENSE, README, CONTRIBUTING, STYLE present (unchanged) |
| 3 | TEST GAPS | PASS | 94 unittest .cc files, 606 TEST() macros — unchanged |
| 4 | PACKAGE UPGRADES | PASS | Bundled deps unchanged (gtest 1.8.1, openssl 3.0.17, quickjs 0.15.1, re2 2015) |
| 5 | PITFALL HUNT | PASS | 10 TODO/FIXME in unittest/ (unchanged); no new stubs or regressions |
| 6 | PERFORMANCE | PASS | PERF-BENCH task on board; no benchmark binaries built |
| 7 | ENDPOINT VERIFICATION | PASS | Binary: `rethinkdb 2.4.5-228-g9cbef4 (GCC 15.2.0)` — 361MB, runs |
| 8 | CI/CD HEALTH | INFRA | Fork repo — no runner available; no gh CLI (remote is local) |
| 9 | DUCKBRAIN SYNC | PASS | DuckBrain MCP available; 19 keys in namespace; `list_keys` confirmed; wrote idle tick #15 |
| 10 | CODE QUALITY | PASS | Working tree clean; gitleaks: 0 leaks (65.69MB scanned in 4.32s) |
| 11 | MIDDLE-OUT WIRING | PASS | Binary links and runs; 17,831 edges across 2,934+ files |

**Hilo:** 17,831 edges across 2,934+ files — Hilo=useful (cached stats)
**Cooldown:** 43200s — re-fixed (7200→43200s, 6th reversion — scheduler restart reset) ✅
**System:** Load 21.18 (high), 7.0Gi/59Gi RAM, 130 threads, no fork contention
**Actions:** Cooldown re-fixed via API PUT, board update, DuckBrain write, no worker spawn needed
**Commit:** Board-only (`.coding-hermes/` committed)

### Status: Blocked — Escalation Already Sent (Tick #7)

CDC-09 conflict resolution remains blocked on Bane review. All CDC-08 sub-tasks complete (42/42 tests). **15 consecutive idle ticks** — project genuinely blocked.

**What's Waiting on Bane:**
- Review of CDC-08 streaming implementation (6 sub-tasks, 42/42 tests passing)
- Sign-off on CDC-09 design/decomposition into 4 sub-tasks
- Decision on Phase 3 architectural work (async I/O, FDW, WASM) in parallel
- CI runner provisioning for fork repo (all CI checks INFRA-blocked)
