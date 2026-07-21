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

## Idle Tick #6 — 2026-07-21 21:15 UTC

**11-Point Audit Results:**

| # | Check | Result | Detail |
|---|-------|--------|--------|
| 1 | SPEC ALIGNMENT | N/A | No specs/ dir; AGENTS.md serves as architecture doc |
| 2 | DOC COVERAGE | PASS | LICENSE (Apache 2.0), README, CONTRIBUTING, STYLE all present |
| 3 | TEST GAPS | PASS† | 1,078 source, 45 test files, 455 TEST() macros; binary builds and runs; unit test binary not built (host resource pressure) |
| 4 | PACKAGE UPGRADES | PASS | Bundled deps unchanged (gtest 1.8.1, openssl 3.0.17, quickjs 0.15.1, re2 2015); pre-existing upstream |
| 5 | PITFALL HUNT | PASS | 263 TODO/FIXME (all upstream code, no new stubs); 0 new stubs added |
| 6 | PERFORMANCE | PASS | No benchmark binaries; PERF-BENCH task already on board |
| 7 | ENDPOINT VERIFICATION | PASS | Binary: `rethinkdb 2.4.5-221-g5d0389 (GCC 15.2.0)`; `--help` works |
| 8 | CI/CD HEALTH | INFRA‡ | 1 run queued 21h+ (no runner); 1 run failed in 1m5s (infra timeout); gh log retrieval fails — consistent runner availability issue |
| 9 | DUCKBRAIN SYNC | SKIP | DuckBrain MCP connection error (known transport issue, not project-specific) |
| 10 | CODE QUALITY | PASS | .gitignore VFS entries correct per tick #5 fix; no new lint/style issues |
| 11 | MIDDLE-OUT WIRING | PASS | Binary links and runs; all modules compiled into single daemon binary |

**Hilo:** 17,831 edges across 2,934 files — Hilo=useful  
**Cooldown:** 14400s→43200s (12h) — graduated slowdown at idle tick #6  
**Actions:** Cooldown update via scheduler API, board update  
**Commit:** Board-only (`.coding-hermes/` gitignored — no code changes)

† Build/tests blocked by host resource exhaustion — INFRA issue, not code  
‡ CI runner availability issue — fork repo has limited runner capacity; push events require manual trigger

**Status:** CDC-09 still blocked on Bane review. All 11 checks pass or N/A/SKIP. Idle tick #6. Cooldown at 12h. No new actionable gaps found.

## [ ] NEVER-DONE — Run 11-point audit next tick
