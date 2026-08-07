# RethinkDB Time-Series Extension (PHASE3)

**Status: in development — QUEUE #3 of PHASE3 (TS-1..TS-6).** This document
tracks the extension as implemented in this fork. The upstream design spec
lives at `.coding-hermes/specs/phase3-timeseries.md`; this file is the
user-facing companion.

## Overview

Time-series workloads stress a general-purpose B-tree: append-heavy writes on a
monotonic time key, range scans over contiguous time windows, and periodic bulk
deletion of aged data. This fork adds native time-series awareness to the
storage engine without giving up the existing B-tree abstraction:

| Feature | Description | Table option | Status |
|---------|-------------|--------------|--------|
| Time-ordered storage | Chunked, append-optimized leaf pages | `timeSeries` | ✅ Implemented (TS-1, TS-2) |
| Read pruning | `between()` on the time field scans only overlapping chunks | `timeSeries` | ✅ Implemented (TS-3) |
| Retention TTL | Automatic deletion of expired chunks | `retention` inside `timeSeries` | 🚧 In progress (TS-4) |
| Downsampling | Continuous aggregate rollups | `downsample` inside `timeSeries` | ⏳ Queued (TS-5) |
| Cluster integration | Chunk index replicated via Raft metadata | — | ⏳ Queued (TS-6) |

## Creating a time-series table

Pass a `timeSeries` object to `tableCreate`:

```javascript
r.tableCreate("sensor_readings", {
  primary_key: "id",
  timeSeries: {
    field: "timestamp",          // required: document field holding the time
    chunk_interval: 3600,        // optional, seconds; default 3600 (1h), min 1
    retention: 7776000,          // optional, seconds (90d); 0 = no retention
    downsample: [                // optional, queued in TS-5
      { age: 86400, to: 60, aggregate: { avg_temp: r.avg("temperature") } }
    ]
  }
})
```

Key points (as implemented):

- **`field` is required** and must be a non-empty string. It names the document
  field whose values must be time pseudo-types.
- **`chunk_interval` and `retention` are integer seconds** (numbers, not
  duration strings). `chunk_interval` must be ≥ 1. `retention` must not exceed
  the maximum of 365 days (`TIME_SERIES_RETENTION_EXCEEDED` otherwise).
- **`downsample` is an array** of `{age, to, aggregate}` objects (`age` and `to`
  in seconds, `aggregate` an object of deterministic ReQL aggregate
  expressions). Parsing and validation exist; the background merge pipeline is
  TS-5.

## Inspecting configuration

The table config exposes the time-series settings:

```javascript
r.table("sensor_readings").config()("time_series")
// → { field: "timestamp", chunk_interval: 3600, retention: 7776000,
//     downsample: [...] }
```

Chunk-level information (chunk count, total rows, per-chunk bounds) is
available through the server's index/status responses (`chunk_count`,
`total_rows`, `chunks`, `newest` are merged across shards by the
time-series status path in `src/rdb_protocol/protocol.cc`).

## Query semantics

- **`between()` on the time field is pruned by the chunk index** (spec §4.2,
  implemented TS-3 in `src/rdb_protocol/store.cc`). Only chunk roots
  overlapping `[start, end)` are scanned; rows are filtered to the exact
  window inside the traversal. `normalize_ts_range` maps ReQL time bounds
  (`MINVAL`/`MAXVAL`/time values) to microsecond windows.
- **Non-time-field queries** scan all chunks (no pruning benefit).
- **`getAll`/primary-key lookups** pass through the standard B-tree path.
- **Write path** (TS-2): inserts route to the current (newest) chunk with
  append-optimized leaves; backfilled/out-of-order writes within the chunk
  window use the standard insert path. Chunks seal when they exceed the size
  threshold.

## Error surface

| Error | Meaning |
|-------|---------|
| `TIME_SERIES_FIELD_MISSING` | Document lacks the configured time field |
| `TIME_SERIES_FIELD_INVALID_TYPE` | Time field is not a time pseudo-type |
| `TIME_SERIES_CONFIG_INVALID` | Malformed `timeSeries` optarg |
| `TIME_SERIES_CONFIG_IMMUTABLE` | Attempt to alter an existing time-series config |
| `TIME_SERIES_RETENTION_EXCEEDED` | `retention` > 365 days |
| `TIME_SERIES_BOUND_INVALID` | Invalid `between()` bound on the time field |
| `TIME_SERIES_CHUNK_CORRUPT` | Chunk index points at an invalid block |
| `TIME_SERIES_DOWNSAMPLE_CONFLICT` | Overlapping downsample age ranges |
| `TIME_SERIES_OUT_OF_ORDER_WINDOW` | Write outside the allowed out-of-order window |
| `TIME_SERIES_CHUNK_OVERFLOW` | Single chunk exceeds size limit (sealed early) |

## Roadmap status

| Queue | Work | Status |
|-------|------|--------|
| TS-1 | Config layer: `time_series_config_t`, serialization, `tableCreate` optarg parse + validation, `config()` exposure | ✅ Done |
| TS-2 | Chunked B-tree storage + append-optimized write path (§4.1/§5.1/§6.2) | ✅ Done |
| TS-3 | Read pruning via chunk index (§4.2/§6.3) — `between()` scoped reads | ✅ Done |
| TS-4 | Retention TTL + background jobs (§5.2/§6.4/§7) | 🚧 In progress |
| TS-5 | Downsample pipeline + planner auto-selection (§4.3/§5.3/§6.4) | ⏳ Queued |
| TS-6 | Cluster integration + benchmarks + chaos (§6.4/§8/§10) | ⏳ Queued |
| PHASE3-FDW | Foreign data wrapper support | ⏳ Queued |
| PHASE3-ASYNC | Async I/O subsystem (PG18-style) | ⏳ Queued |
| PHASE3-WASM | WASM-based UDF sandbox | ⏳ Queued |

## Implementation notes

- Core types: `src/btree/time_series_config.hpp` (`time_series_config_t`,
  `downsample_step_t`), `src/btree/time_chunk.hpp` (`time_chunk_index_t`,
  `time_chunk_bounds_t`), catalog in `src/btree/time_series_ops.{hpp,cc}`.
- Optarg parsing + config formatting: `src/rdb_protocol/terms/time_series.cc`.
- Write/read wiring: `src/rdb_protocol/store.cc`, `src/rdb_protocol/real_table.cc`.
- Error constants: `src/rdb_protocol/time_series_errors.hpp`.
- Integration tests: `test/ts2_e2e_probe.py`, `test/ts3_e2e_probe.py`.
