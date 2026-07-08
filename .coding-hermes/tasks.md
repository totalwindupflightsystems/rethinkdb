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
- [x] Cherry-pick marchon's security fixes from PR #7191
  - [x] Security and stability fixes (22 files): timing-safe auth, cJSON buffer overflow, SASLPrep hardening, PBKDF2 iterations, null pointer derefs, unsigned underflows, uninitialized vars, signed/unsigned mismatches
  - [x] Critical issue fixes: #6880 cluster crash, #7124 ARM crash, #6433 allocation bounds, #7005 datum bounds, #6952 RISC-V, #7120 /proc/meminfo
  - [x] Container support: cgroup memory detection, shard limit 64→256, shutdown guarantee fix
  - [x] Skipped: JS engine feature (pluggable V8/QuickJS/Duktape/Hermes), AI-generated reports, NOTES.md updates
- [x] Drop AI-generated report files from PR #7191
  - [x] Removed 8 AI-generated files: build/cluster/crash/memory/other JSON, issue_analysis_report.md, rethinkdb_open_issues
- [x] Audit all CVEs in bundled deps (Boost 1.60, OpenSSL, QuickJS) → see .coding-hermes/research/cve-audit-bundled-deps.md
  - [x] **CRITICAL:** Upgrade OpenSSL 3.0.7 → 3.0.17 (20+ CVEs, all TLS endpoints affected) — committed f75f9fd
  - [x] **HIGH:** Upgrade QuickJS to quickjs-ng v0.15.1 (CVE-2023-48184 CVSS 9.8, CVE-2024-13903, CVE-2026-0822) — migrated from unmaintained rethinkdb/quickjspp fork to actively maintained quickjs-ng/quickjs; committed 68f7f3a
  - [x] **LOW:** Upgrade Boost 1.60 → 1.85+ (no known CVEs in used components, code-quality improvement)
- [x] Modernize C++ standard (C++11 → C++17/20)
  - [x] CP1: Change `-std=gnu++0x` → `-std=gnu++17` in src/build.mk and configure script
  - [x] CP2: Update CPPLINT.cfg to allow C++17 features (remove `-build/c++17` filter)
  - [x] CP3: Verify clean build with GCC 15 and update CI if needed
  - [x] CP4: Audit code for deprecated C++11 constructs that now warn under C++17
    - [x] Migrated 10 `std::result_of<>` → `std::invoke_result<>` across 5 files (incremental_lenses.hpp, range_map.hpp, region_map.hpp, watchable.hpp, watchable.tcc)
    - [x] Verified no `register` keyword, `std::auto_ptr`, `std::tr1`, or `std::unary_function` usage
- [ ] Replace deprecated Python 2 scripts with Python 3
- [ ] Add RISC-V CI target (PR #7194)
- [ ] Full-text search: GIN-style indexes with stemming/tokenization
- [ ] Vector index: HNSW/IVFFlat for embeddings (pgvector parity)
- [ ] BRIN-like sparse indexes for time-series/append-only workloads

## Phase 3: v3.0 (Future)
- [ ] Declarative table partitioning
- [ ] Parallel query execution
- [ ] Logical replication / CDC streaming
- [ ] Async I/O subsystem (PG18-style)
- [ ] JSONB/JSONPath improvements
- [ ] Generated/virtual columns
- [ ] MERGE/UPSERT with complex conditions
- [ ] Time-series optimizations
- [ ] Foreign data wrapper support
- [ ] WASM-based UDF sandbox (replace V8/QuickJS with WASM runtime)
