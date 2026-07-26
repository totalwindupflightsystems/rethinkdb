<!--
  ⚠️  BOARD FORMAT — coding-hermes-model-router v1.3 (2026-07-24)
  All tasks MUST use matrix format: | ID | Task | Pri | Cpx | Deps | Tags | Model | Reasoning | Fallback |
  Before editing this file, load the skill: skill_view(name='coding-hermes-model-router')
  Validate: python3 ~/.hermes/scripts/validate-board-format.py .coding-hermes/tasks.md
- [ ] **GITREINS-JUDGE — Configure LLM evaluator for commit quality review**
  | 🔴 Critical | — | — | deepseek-v4-flash @ deepseek-foreman | GITREINS_LLM_API_KEY in ~/.hermes/.env | foreman-direct |

  Run: `python3 ~/.hermes/scripts/check-gitreins-judge.py .` to verify.
  Default limits (adjust per-project based on codebase size and task complexity):
  - Fast/small projects: `max_iterations: 50`, `max_time: 10m`, tokens: `0.2M/0.4M`
  - Large repos (Go monorepos, 100+ files): `max_iterations: 100`, `max_time: 30m`, tokens: `1M/2M`
  - C++/Rust (slow compiles): `max_time: 30m` minimum
  - Scheduler/production infra: `max_time: 30m`, tokens: `1M/2M`
  Supervisor auto-flags projects where limits are too low for codebase size.

| 🔴 Critical | — | — | deepseek-v4-flash @ deepseek-foreman | GITREINS_LLM_API_KEY in ~/.hermes/.env | foreman-direct |

  Run: `python3 ~/.hermes/scripts/check-gitreins-judge.py .` to verify.
  If missing, create/edit .gitreins/config.yaml with evaluator section using deepseek-v4-flash.
  This is CRITICAL for code quality — no automated review of worker output without it.

  NEVER remove the matrix header row or NEVER-DONE / E2E-001 fixtures.
-->

# RethinkDB — Model Router Task Matrix

**Core purpose:** Modernize and extend RethinkDB — a distributed, open-source NoSQL database for realtime web apps — with CDC streaming, parallel queries, partitioning, and C++17 modernization.

## Active Tasks

- [ ] **E2E-001 — E2E Testing Tick (self-improving loop)** 🔁 Recurring every 5-10 ticks
  Spawn Luna (browser/screenshots) or Step 3.7 Flash (CLI/API). Deploy/build, Playwright, screenshots, endpoints, console. → e2e-output/tasks.md → inject into board. See foreman Step 1.5i. Proven: HEADING 10 bugs found.

| ID | Task | Priority | Complexity | Deps | Tags | Model | Reasoning | Fallback |
|----|------|----------|------------|------|------|-------|-----------|----------|
|~~CDC-09~~ | ✅ Conflict resolution (LWW, PK-merge, custom handler, conflict log) — 4/4 subs done | Critical | 7 (new subsystem) | CDC-08 | +++backend, +++distributed-systems, ++architecture | GPT-5.6 Sol | Greenfield ~600 lines in clustering/rdb_protocol; architectural decisions; distributed state | GLM-5.2 |
|~~CDC-09a~~ | ✅ LWW resolver + tombstone versions | High | 4 | CDC-09 | ++code-generation, +architecture | DeepSeek V4 Pro | ~150 lines; bounded deterministic logic | GLM-5.2 |
|~~CDC-09b~~ | ✅ Primary-key merge | High | 4 | CDC-09a | ++code-generation, +testing | DeepSeek V4 Pro | ~120 lines; upsert logic well-specified | GLM-5.2 |
|~~CDC-09c~~ | ✅ Custom handler + validation | Medium | 5 | CDC-09a | ++code-generation, ++security, +concurrency | GLM-5.2 | ~180 lines; restricted ReQL evaluation; security boundary | DeepSeek V4 Pro |
|~~CDC-09d~~ | ✅ Conflict log + operator actions | Medium | 4 | CDC-09c | ++code-generation, +testing | DeepSeek V4 Pro | ~150 lines; durable log + operator retry/skip/resolve | GLM-5.2 |
|~~CDC-10~~ | ✅ CDC comprehensive tests (4 commits, 69 new tests, 131/131 PASS, GitReins judge 5/5) | High | 6 | CDC-09 | +++testing, ++debugging, +concurrency | DeepSeek V4 Pro | Unit, integration, failure, durability, performance, compatibility tests — DONE | GLM-5.2 |
| INT-01 | **⚡ IN PROGRESS** — Integration test harness + fixtures + server lifecycle | Critical | 3 | — | ++testing, ++infrastructure | DeepSeek V4 Pro | Python harness, server start/stop/restart, fixture infrastructure | GLM-5.2 |
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

## Productive Tick #27 — 2026-07-25 20:50 UTC

**14-Point Audit — 27th tick (CDC-10 VERIFIED ✅, INT-01 dispatched):**

| # | Check | Result | Detail |
|---|-------|--------|--------|
| 1 | SPEC ALIGNMENT | N/A | No specs/ dir; AGENTS.md serves as architecture doc |
| 2 | DOC COVERAGE | PASS | SECURITY.md, CODE_OF_CONDUCT.md, SUPPORT.md all present (fixed tick #25) |
| 3 | TEST GAPS | PASS | **716 TEST() macros across 95 unit-test .cc files (unchanged from tick #26 — stable)** |
| 4 | PACKAGE UPGRADES | PASS | Bundled deps unchanged (gtest 1.8.1, openssl 3.0.17, quickjs 0.15.1, re2 2015) |
| 5 | PITFALL HUNT | PASS | 700 TODO/FIXME (pre-existing large codebase, no new regressions) |
| 6 | PERFORMANCE | PASS | PERF-BENCH task on board; 0 benchmark files — unchanged |
| 7 | ENDPOINT VERIFICATION | PASS | Binary: `rethinkdb 2.4.5-258-g3a4432-dirty (GCC 15.2.0)` — ELF 64-bit, links and runs |
| 8 | CI/CD HEALTH | INFRA | Fork repo — no runner available; local-only |
| 9 | DUCKBRAIN SYNC | FIXED | Populated tick-27 entry + CDC-10 judge verification. 6 entries now in rethinkdb namespace |
| 10 | CODE QUALITY | PASS | Gitleaks: 0 leaks (66MB in 2.54s). GitReins judge: CDC-10 ALL 5 criteria PASS |
| 11 | MIDDLE-OUT WIRING | PASS | 17,894 edges across 2,943 files — Hilo=useful |
| 12 | USABILITY | SKIP | Database engine — no browser/UI. Binary smoke: `--version` OK |
| 13 | E2E TESTING | GAP | No e2e-output/ directory exists. E2E-001 task on board but never triggered |
| 14 | GITREINS JUDGE | PASS | CDC-10 evaluation: 5/5 criteria PASS (cdc_sink 41 tests, cdc_failure 25 tests, cdc_durability 14 tests, new_terms proto IDs 198-215, 131/131 tests PASS). Caps: 100/30m/1M/2M |

**🔥 CDC-10 VERIFIED:** Worker produced **4 commits** with **1,668 lines** of comprehensive CDC tests across 4 files. GitReins Tier 2 LLM evaluator confirmed all 5 criteria with detailed file:line evidence. 131/131 CDC tests PASS in 330ms.

**Hilo:** 17,894 edges across 2,943 files — Hilo=useful  
**System:** Load 7.04, 11Gi/59Gi RAM, 47Gi available, 16 CPUs, up 9d 8h  
**Cooldown:** 43200s (12h) — holding stable ✅

**Actions this tick:**
1. ✅ Verified CDC-10 worker output — 4 commits, 1,668 lines, 4 new test files
2. ✅ Ran 131 CDC tests — all PASS (330ms)
3. ✅ Created GitReins task CDC-10 — judge evaluated 5/5 criteria PASS
4. ✅ Completed GitReins task CDC-10 (all criteria verified with file:line evidence)
5. ✅ Populated DuckBrain with tick-27 + CDC-10 judge verification entries
6. ✅ Dispatched INT-01 worker (DeepSeek V4 Pro @ openrouter) — integration test harness
7. ✅ Updated test count: 716 TEST() macros across 95 unit-test .cc files

### Status: INT-01 dispatched — integration test harness in progress

CDC subsystem is DONE and VERIFIED (CDC-09a-d + CDC-10 = 8 sub-tasks, all ✅ with GitReins judge). The integration test pipeline (INT-01 through INT-08) is the active workstream.

**INT-01 worker dispatched:** Python integration test harness with server lifecycle, fixtures, driver helpers, and 5 health tests.

**Next tick:** Poll INT-01 worker. If complete, verify test harness works, run GitReins judge, then dispatch INT-02 (CRUD) or INT-03 (Changefeed CDC).

**Execution order:** INT-01 → INT-02 (CRUD) → INT-03 (Changefeed CDC) → INT-04 (Index + durability) → INT-05 (Bulk + edge cases) → INT-06 (CDC end-to-end) → INT-07 (Vector/FTS/BRIN) → INT-08 (CI integration) → PERF-BENCH

**Cooldown:** 43200s (12h) — holding stable
