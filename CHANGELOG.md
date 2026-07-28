# RethinkDB Changelog

## 2.4.5-276 (2026-07-27) — CDC + Vector/FTS + BRIN

### Added
- **CDC (Change Data Capture):** Publication/subscription/sink system for real-time data streaming (CDC-05 through CDC-10)
  - Conflict resolution: LWW, PK-merge, custom handler, conflict log (CDC-09a-d)
  - 131 unit tests + 29 integration tests + 24 CDC e2e tests
- **Vector indexes:** L2, cosine, and inner product distance metrics (INT-07)
  - HNSW graph construction with persistence
  - 42 integration assertions across 10 test functions
- **Full-Text Search (FTS):** Match queries via ReQL surface (INT-07)
  - Multi-field indexing and query support
- **BRIN indexes:** Block Range INdex for range queries (INT-07-BUG-BRIN)
  - Index creation functional; ready-state transition under investigation
- **Performance benchmarks:** 30 benchmark tests for CDC, vector, FTS, BRIN (PERF-BENCH)
  - CDC: 106M records/sec, HNSW: 15K queries/sec, BRIN: 7.7M entries/sec
- **CI/CD workflow:** GitHub Actions build on gcc-15 / ubuntu-26.04 (INT-08)

### Fixed
- HNSW graph construction crash — buf_lock lifetime bug in protocol.cc (tick #34)
- BRIN sidecar construction — switched from sindex B-tree to primary B-tree traversal

### Known Limitations
- BRIN indexes create successfully but remain in ready=False state; index_wait hangs
  - Root cause diagnosed: sidecar transaction undoes construction loop ready-state
  - Vector and FTS indexes work correctly

## 2.4.5 (upstream)
- TLS encryption for driver, intracluster, and web UI connections
- User authentication and permissions system
- See upstream release notes for full details
