# RethinkDB — Steward Task Queue

## Phase 1: Foundation (Now)
- [ ] Merge `v2.4.x` into `main` (unreleased v2.4.5 prep)
- [ ] Cut and publish v2.4.5 release
- [ ] Fix `.gitreins` config (lint command, test command)
- [ ] Set up Hilo for codebase navigation
- [ ] Modernize CI (add GCC 14/15, Clang 18/19, ARM64)

## Phase 2: v2.5 (Next)
- [ ] Cherry-pick marchon's security fixes from PR #7191
- [ ] Drop AI-generated report files from PR #7191
- [ ] Fix all compiler warnings on modern GCC/Clang
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
