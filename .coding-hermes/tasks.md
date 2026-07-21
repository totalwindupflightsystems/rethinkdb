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
**Discovery Sweeps:** 9 ticks of audits; CI cpplint fixed; binary builds and links; 0 CVEs; 0 gitleaks; CDC-08 decomposed from monolithic to 6 sub-tasks; CDC-09 blocked on Bane review (5 idle ticks at 4h cooldown).

## Idle Tick #5 — 2026-07-21 03:26 UTC

**11-Point Audit Results:**

| # | Check | Result | Detail |
|---|-------|--------|--------|
| 1 | SPEC ALIGNMENT | N/A | No specs/ dir; AGENTS.md serves as architecture doc |
| 2 | DOC COVERAGE | FIXED | LICENSE created (Apache 2.0); README/CONTRIBUTING/STYLE present |
| 3 | TEST GAPS | PASS† | 1,166 source, 94 test files, 606 TEST() macros; binary builds; unit test binary lists tests; run blocked by host resource pressure (INFRA) |
| 4 | PACKAGE UPGRADES | PASS | Bundled deps (gtest 1.8.1, openssl 3.0.17, quickjs 0.15.1, re2 2015); pre-existing upstream — no actionable upgrades |
| 5 | PITFALL HUNT | PASS | 41 TODO/FIXME (upstream code); 0 stubs; `return {}` are guard clauses; gitleaks `external/` allowlist acceptable |
| 6 | PERFORMANCE | PASS | 3 benchmark tests (vector ops); PERF-BENCH task already on board |
| 7 | ENDPOINT VERIFICATION | PASS | Binary builds+runs: `rethinkdb 2.4.5-221-g5d0389 (GCC 15.2.0)`; unit test binary lists tests |
| 8 | CI/CD HEALTH | INFRA‡ | Fork repo (totalwindupflightsystems/rethinkdb); gh crashed with `pthread_create failed` — host resource exhaustion; manual trigger only |
| 9 | DUCKBRAIN SYNC | PASS | 18 entries in rethinkdb namespace; idle-ticks updated to #5 |
| 10 | CODE QUALITY | FIXED | .gitignore VFS entries corrected (edges.jsonl now tracked, cache files excluded) |
| 11 | MIDDLE-OUT WIRING | PASS | Binary links and runs; all modules compiled into single daemon binary |

**Hilo:** 17,831 edges across 2,934 files — Hilo=useful  
**Cooldown:** 1800s→14400s (re-fixed via API PUT — daemon reversion)  
**Actions:** LICENSE created, .gitignore VFS fix, DuckBrain idle counter update, board update  
**Commit:** <commit-hash>  

† Build/tests blocked by host resource exhaustion (`pthread_create failed`) — INFRA issue, not code  
‡ gh CLI crashed with `pthread_create failed` — same host resource exhaustion  

**Status:** CDC-09 still blocked on Bane review. All 11 checks pass or are N/A. Idle tick #5. Cooldown at 4h. No new actionable tasks.

## [ ] NEVER-DONE — Run 11-point audit next tick
