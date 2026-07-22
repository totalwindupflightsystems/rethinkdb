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
**U01 Usability Audit:** Full audit completed tick #11 — 447 source files, 41 test files (606 TEST macros), binary builds+links, HTTP admin routes wired (ajax/reql + static), CDC streaming code verified (replication_mailbox, cdc_types, 9 perfmon counters), error handling: 3,749 patterns across codebase, no new stubs or regressions. Project is stable; blocked only on Bane's CDC-08 review.
**Discovery Sweeps:** 11 ticks of audits; CI cpplint fixed; binary builds and links; 0 CVEs; 0 gitleaks; CDC-08 decomposed from monolithic to 6 sub-tasks; CDC-09 blocked on Bane review (13 idle ticks at 12h cooldown).

## Idle Tick #13 — 2026-07-22 08:47 UTC

**11-Point Audit (Quick Check) — 13th consecutive idle tick:**

| # | Check | Result | Detail |
|---|-------|--------|--------|
| 1 | SPEC ALIGNMENT | N/A | No specs/ dir; AGENTS.md serves as architecture doc |
| 2 | DOC COVERAGE | PASS | LICENSE, README, CONTRIBUTING, STYLE present (unchanged) |
| 3 | TEST GAPS | PASS | 94 unittest .cc files, 606 TEST() macros; binary: `2.4.5-228-g9cbef4 (GCC 15.2.0)` — 345MB |
| 4 | PACKAGE UPGRADES | PASS | Bundled deps unchanged (gtest 1.8.1, openssl 3.0.17, quickjs 0.15.1, re2 2015) |
| 5 | PITFALL HUNT | PASS | 263 upstream TODO/FIXME (unchanged); no new stubs or regressions |
| 6 | PERFORMANCE | PASS | PERF-BENCH task on board; no benchmark binaries built |
| 7 | ENDPOINT VERIFICATION | PASS | Binary runs; `--version` returns `2.4.5-228-g9cbef4` |
| 8 | CI/CD HEALTH | INFRA | Fork repo — no runner available; cooldown reversion #4 |
| 9 | DUCKBRAIN SYNC | UPDATED | Idle tick counter written to DuckBrain (tick #13) |
| 10 | CODE QUALITY | PASS | .gitignore VFS entries correct; gitleaks `○` (known glob-panic issue); working tree clean |
| 11 | MIDDLE-OUT WIRING | PASS | Binary links and runs; all modules compiled into single daemon binary |

**Hilo:** 17,831 edges across 2,934+ files — Hilo=useful  
**Cooldown:** 7200→43200s (reverted by scheduler restart #4, re-fixed via API PUT; GET confirms 43200)  
**Actions:** DuckBrain idle counter update (#13), cooldown re-fix, board update  
**Commit:** Board-only (`.coding-hermes/` committed)

### ⚠️ Cooldown Reversion #4 — Recurring Scheduler Daemon Restart Pattern

This is the **fourth consecutive tick** where the scheduler daemon restart has reverted the cooldown from 43200s back to a lower value (varied: 1800s→7200s this time). Per `cooldown-reset-on-restart.md`, this will keep happening until the root cause is addressed at the scheduler daemon level (fleet TOML config overwrites API-set cooldown on startup via `ApplyFleetConfig`). The foreman applied the fix via `PUT /api/v1/projects/rethinkdb {"CooldownS":43200}` — GET `{"project":{"CooldownS":43200,"Enabled":true}}` confirms it took effect.

### Status: Blocked — Escalation Already Sent (Tick #7)

CDC-09 conflict resolution remains blocked on Bane review of the prior CDC-08 streaming implementation and design specs. All CDC-08 sub-tasks complete with 42/42 tests. No new work can proceed until Bane signs off. **13 consecutive idle ticks** with zero new actionable gaps — the project is genuinely blocked, not undiscovered work.

#### What's Waiting on Bane:
- Review of CDC-08 streaming implementation (6 sub-tasks, 42/42 tests passing)
- Sign-off on CDC-09 design/decomposition into 4 sub-tasks (LWW resolver, PK-merge, custom handler, conflict log)
- Decision on whether to proceed with Phase 3 architectural work (async I/O, FDW, WASM) in parallel
- CI runner provisioning for fork repo (all CI checks are INFRA-blocked)
