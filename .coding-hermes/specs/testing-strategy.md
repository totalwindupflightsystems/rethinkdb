# Phase 3 Integration Testing — Design Specification

Status: Active · Priority: Critical · Owner: Foreman

## 1. Problem

22,000 lines of Phase 3 code across 7 modules (CDC, FTS, Vector, BRIN,
Partitioning, Parallel Query, Async I/O). All tests are in-memory unit tests.
Zero end-to-end tests exercise the actual server, ReQL query path, B-tree
integration, Raft metadata flow, or client-server protocol.

Two real bugs (base64 off-by-one, optargs whitelist gaps) were discovered in
the first 5 seconds of live testing — neither had any unit test coverage.

## 2. Testing Tiers

### Tier 1 — Unit Tests (existing, maintained)
- `src/unittest/*.cc` — in-memory, no server
- Coverage: types, serialization, resolver logic, algorithms
- Run: `./build/release/rethinkdb-unittest --gtest_filter='*'`
- Target: 100+ tests, all pass on every commit

### Tier 2 — Integration Tests (this spec)
- `test/cdc_integration_test.py` — Python driver, real server
- Coverage: ReQL query path, write capture, changefeeds, indexes, durability
- Run: starts server, connects client, exercises full stack
- Target: 5 test suites, 20+ assertions, clean teardown

### Tier 3 — Cluster Tests (future)
- `test/cdc_cluster_test.py` — 2+ node cluster
- Coverage: Raft metadata, cross-node publications, subscriptions, replication
- Requires: 2+ rethinkdb instances, coordinated startup
- Target: post-CDC-10

### Tier 4 — Performance Benchmarks (future)
- `src/benchmark/cdc_bench.cc` — Google Benchmark
- Coverage: write throughput with CDC enabled, changefeed latency, slot GC
- Target: post-CDC-10

## 3. Integration Test Suite

### 3.1 Architecture

```
test/
├── cdc_integration_test.py     # Main integration test harness
├── fixtures/
│   ├── __init__.py
│   ├── server.py                # RethinkDB server lifecycle (start/stop/restart)
│   └── data.py                  # Test data generators
└── results/                     # Test output (.gitignored)
```

### 3.2 Test Suites

| Suite | What it tests | CDC path exercised |
|-------|---------------|-------------------|
| **Basic CRUD** | db_create, table_create, insert, get, update, delete, count, filter | rdb_write_visitor_t, store_t write path |
| **Changefeed** | changes() with include_initial, update delivery with old_val/new_val | cdc_types serialization, changefeed push |
| **Index operations** | index_create, index_wait, between, get_all | sindex_superblock_t, btree integration |
| **Durability** | server restart, data survival | serializer_t commit, B-tree page persistence |
| **Bulk & edge** | 500-doc batch inserts, bulk updates, empty operations, deletes | write path stress, buffer cache eviction |
| **CDC-specific** (Tier 3) | publication_create, subscription_create, cdc_sink_create | full CDC pipeline, mailbox RPC, Raft metadata |

### 3.3 Exit Criteria per Suite

Every suite must:
1. Start a clean server
2. Exercise the feature through the Python ReQL driver
3. Assert correct results (not just "no crash")
4. Verify changefeed delivery where applicable
5. Clean up (db_drop) on both success and failure paths
6. Pass on GCC 15.2.0 release build
7. Complete in < 60 seconds

## 4. Board Tasks

| ID | Task | Tests | Priority | Blocked by |
|----|------|-------|----------|------------|
| **INT-01** | Integration test harness + fixtures | — | Critical | — |
| **INT-02** | Basic CRUD suite | 5 | Critical | INT-01 |
| **INT-03** | Changefeed CDC path suite | 5 | Critical | INT-01 |
| **INT-04** | Index + durability suite | 10 | High | INT-02 |
| **INT-05** | Bulk + edge case suite | 5 | High | INT-02 |
| **INT-06** | CDC end-to-end (publications, subscriptions) | 10 | Critical | CDC-10 |
| **INT-07** | Vector/FTS/BRIN index query tests | 15 | High | INT-01 |
| **INT-08** | CI integration (run on every commit) | — | High | INT-01..INT-07 |

## 5. CI Integration

```yaml
# .github/workflows/build.yml addition
integration:
  runs-on: ubuntu-latest
  steps:
    - uses: actions/checkout@v4
    - run: ./configure --allow-fetch && make -j4
    - run: pip install rethinkdb && python3 test/cdc_integration_test.py
```

## 6. Regression Guard

Every bug found during integration testing must have:
1. A failing integration test case added to the appropriate suite
2. A unit-level regression test in `src/unittest/` (where possible)
3. A commit message referencing both the integration and unit test additions

Example (from the two bugs already found):
- `fix: base64 off-by-one (size >= 3)` → `test: base64 regression (9 tests)` → `test: cdc_integration changefeed delivery`
- `fix: optargs whitelist gaps` → `test: optargs regression (6 tests)` → `test: cdc_integration table_create`

## 7. Success Criteria

A Phase 3 feature is "done" when:
- [ ] Unit tests pass (Tier 1)
- [ ] Integration tests pass (Tier 2)  
- [ ] At least one changefeed-based assertion verifies CDC write capture
- [ ] Data survives a server restart
- [ ] Index operations work end-to-end
- [ ] Regression tests exist for any bugs found during testing
