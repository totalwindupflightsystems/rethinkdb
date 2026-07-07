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
- [ ] Cherry-pick marchon's security fixes from PR #7191
- [ ] Drop AI-generated report files from PR #7191
- [ ] Audit all CVEs in bundled deps (Boost 1.60, OpenSSL, QuickJS)
- [ ] Modernize C++ standard (C++11 → C++17/20)
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
