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

## Idle Tick #23 — 2026-07-24 08:00 UTC

**11-Point Audit (Quick Check) — 23rd consecutive idle tick:**

| # | Check | Result | Detail |
|---|-------|--------|--------|
| 1 | SPEC ALIGNMENT | N/A | No specs/ dir; AGENTS.md serves as architecture doc |
| 2 | DOC COVERAGE | PASS | LICENSE, README, CONTRIBUTING, STYLE present (unchanged) |
| 3 | TEST GAPS | PASS | 94 unittest .cc files, 606 TEST() macros — unchanged |
| 4 | PACKAGE UPGRADES | PASS | Bundled deps unchanged (gtest 1.8.1, openssl 3.0.17, quickjs 0.15.1, re2 2015) |
| 5 | PITFALL HUNT | PASS | No new TODO/FIXME; no new stubs or regressions since tick #22 |
| 6 | PERFORMANCE | PASS | PERF-BENCH task on board; 0 benchmark files in build/ |
| 7 | ENDPOINT VERIFICATION | PASS | Binary: `rethinkdb 2.4.5-246-g3006af-dirty (GCC 15.2.0)` — 345MB, ELF 64-bit, build/release/ (rebuilt since tick #22) |
| 8 | CI/CD HEALTH | INFRA | Fork repo — no runner available; local-only |
| 9 | DUCKBRAIN SYNC | PASS | DuckBrain recovered via `hermes mcp test`; wrote tick #23 to namespace (id: 395974bb-6916-4901-bd01-e0acff6a8906) |
| 10 | CODE QUALITY | PASS | Working tree: edges.jsonl dirty (Hilo post-commit artifact), test_disk_backed_queue.create untracked (RethinkDB data file ~288KB). Gitleaks: 0 leaks (87MB scanned in 6.3s) |
| 11 | MIDDLE-OUT WIRING | PASS | Binary links and runs; 17,868 edges across 2,941 files (hilo graph stats confirmed) |

**Hilo:** 17,868 edges across 2,941 files — Hilo=useful (live stats this tick)
**Cooldown:** 43200s — re-fixed (900→43200s, 14th reversion — scheduler restart reset) ✅ VERIFIED via API GET
**System:** Load 8.80, 14Gi/59Gi RAM, 44Gi available, 16 CPUs, up 7d 19h
**Actions:** Cooldown re-fixed via Python API PUT (900→43200s) with GET confirmation (CooldownS=43200). DuckBrain recovered via `hermes mcp test`. DuckBrain write for idle #23. No worker spawn needed.

### Status: Blocked — Escalation Already Sent (Tick #7)

CDC-09 conflict resolution remains blocked on Bane review. All CDC-08 sub-tasks complete (42/42 tests). **23 consecutive idle ticks** — project genuinely blocked.

**What's Waiting on Bane:**
- Review of CDC-08 streaming implementation (6 sub-tasks, 42/42 tests passing)
- Sign-off on CDC-09 design/decomposition into 4 sub-tasks
- Decision on Phase 3 architectural work (async I/O, FDW, WASM) in parallel
- CI runner provisioning for fork repo (all CI checks INFRA-blocked)

