# RethinkDB Fork — Diagnostics Trail

How this fork is built, the errors encountered while using it for real
(2026-08-09 dogfood run), and the right way to do things. This is the record
that answers "does it actually work" from the repo — read
`docs/dogfood/2026-08-09-integration.md` for the user-facing report.

## How the system is built

- **Core engine:** upstream RethinkDB 2.4.x (C++17, cooperative coroutines on
  `arch/runtime`, B-tree storage in `btree/`+`serializer/`, Raft metadata in
  `clustering/`). The stock binary is 346MB ELF; the fork builds with GCC 15.
- **Fork extensions (PHASE3), all living inside the stock architecture:**
  - *Time-series*: `src/btree/time_series_config.hpp` / `time_chunk.hpp` /
    `time_series_ops.{hpp,cc}` — chunked leaf storage with a chunk index;
    `src/rdb_protocol/terms/time_series.cc` parses the `timeSeries` optarg;
    write/read wiring in `src/rdb_protocol/store.cc` (TS-3 pruning: only chunk
    roots overlapping `[start,end)` are scanned). Status merged across shards
    in `src/rdb_protocol/protocol.cc`.
  - *Vector/FTS*: new ReQL terms 198–215 in `src/rdb_protocol/terms/`
    (fts_tokenize/fts_match/vector/vector_near/partition_info/repartition/
    CDC publication/subscription/sink terms), VECTOR pseudo-type disk
    serialization, FTS tokenize-on-write.
  - *CDC*: `src/rdb_protocol/publication.{hpp,cc}`,
    `subscription.{hpp,cc}`, `cdc_sink.{hpp,cc}` + term wrappers; cluster
    side: `src/clustering/replication_coordinator.{hpp,cc}` (slot binding,
    LSN pinning via `retention_->pin_through`) and Raft metadata plumbing in
    `clustering/administration/tables/table_metadata.{hpp,cc}`.
- **Driver:** vendored Python driver at `driver/python3/` with regenerated
  `ql2_pb2.py` from `src/rdb_protocol/ql2.proto`, packaging via
  `pyproject.toml` (setuptools; deps six/protobuf/looseversion; optional
  numpy for vectors).
- **Board/ops:** `.coding-hermes/tasks.md` matrix board; foreman ticks drive
  tasks (currently TS-6 cluster integration + benchmarks in flight).

## Errors hit during the dogfood run (and their true causes)

1. **`The directory ... does not exist, run 'rethinkdb create -d ...'`** —
   expected RethinkDB behavior: `serve` needs an initialized data dir. Not a
   bug; `create` then `serve` is the flow.
2. **`Could not bind to http port ... reserved or already in use` (x2)** —
   host is crowded (8080–8083 taken by unrelated services). RethinkDB exits
   instead of auto-picking a port. Workaround: explicit `--http-port`. The
   first "HTTP 200" check on 8081 was actually ANOTHER app's page — always
   verify the `<title>` when probing ports on shared hosts.
3. **`Cursor.next() got an unexpected keyword argument 'timeout'`** — the
   vendored driver's `next(self, wait=True)` lacks the upstream `timeout=`
   kwarg (net.py:226). Driver drift vs upstream docs (RT-GAP-011). Use
   `next(wait=seconds)`.
4. **FTS index `ready: true` but `fts_match` returns `[]`** — index created
   as `index_create(name, fts=True)` (as the README implies) stores nothing
   matchable; `multi=True` is required so each token becomes an index entry.
   The project's own tests always use `fts=True, multi=True`; the README term
   table omits it (RT-GAP-012). Silent empty results — no error to catch.
5. **CDC objects stuck in `state: 'creating'` forever; target table empty** —
   the big one. Root cause at source level: `publication_state_t::READY`,
   `subscription_state_t::READY`, `cdc_sink_state_t::CONNECTING/STREAMING`
   appear ONLY in enum definitions and `*_to_cstr` string converters. Nothing
   in `src/` ever assigns them; the CDC term files spawn no coroutines/pumps.
   The metadata CRUD surface (create/list/status/drop) is fully wired through
   Raft; the replication/streaming engine that should consume it is not.
   Compounding: the project's own `test/cdc_e2e_test.py::test_cdc_changefeed_delivery`
   does not test delivery — it inserts into the source table and reads back
   from the source table. So INT-06 "24/24" and CLU-E2E "12/12" green results
   assert only CRUD, and the board's "changefeed delivery" claims are
   unbacked (RT-GAP-010). This is the classic premature-completion pattern:
   green tests on the metadata surface, unwired user path.
6. **Probe-side traps (not server bugs):** `filter({'done': False})` after
   update+delete legitimately returned 0 rows (my expectation was stale);
   `between` 2h window at 5-min cadence = 24 docs not 25 (half-open interval
   math); vector_near returns `{dist, doc}` nested shape (undocumented);
   missing-field insert reports the error in the result object, not as an
   exception (standard ReQL write semantics).

## The right way to do things

- **Server lifecycle:** `create -d` once → `serve -d ... --http-port <free>`
  (this host: never rely on 8080). Graceful stop = SIGTERM; restart is clean
  and durable (verified).
- **Driver install:** `pip install ./driver/python3` in a venv (works;
  RT-GAP-002 is stale). Import `from rethinkdb import r`.
- **Time-series:** as in docs/time-series.md — `timeSeries={field, chunk_interval,
  retention, downsample}`; `between(..., index='timestamp')` for pruned reads;
  check `res['errors']` on inserts.
- **Vector:** `index_create(name, vector={'dim':N,'metric':'l2'})` → `index_wait`
  → insert with `r.vector([...])` → `vector_near(name, r.vector([...]), k=N)`,
  read `row['doc']`.
- **FTS:** ALWAYS `index_create(name, r.row[field], fts=True, multi=True)`.
- **CDC:** DO NOT USE yet (RT-GAP-010). If you must probe: the API surface
  works (create/list/status/drop) but nothing streams. Re-check after the
  state machine lands.
- **Changefeeds (the reliable realtime path):** `table.changes()` with
  `feed.next(wait=2)` — push delivery verified.
- **Diagnosing the fork's own claims:** board ✅ rows for CDC say "delivery
  through ReQL path" — verify by writing to the SOURCE and reading the
  TARGET table, not by trusting suite names. The dogfood probes under
  `docs/dogfood/2026-08-09-integration.md` are the reproduction recipe.

## Verification commands (repro in ~5 min)

```bash
./build/release/rethinkdb create -d /tmp/rt-data
./build/release/rethinkdb serve -d /tmp/rt-data --driver-port 28017 \
    --cluster-port 29017 --http-port 18083 --bind all &
python3 -m venv /tmp/rt-venv && /tmp/rt-venv/bin/pip install driver/python3
# then run the probe scripts referenced in docs/dogfood/2026-08-09-integration.md
```
