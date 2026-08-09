# RethinkDB Fork — Integration Report (2026-08-09 dogfood run)

A real consumer integration of this fork: Python driver + live server, done from
scratch in `/tmp/dogfood-rethinkdb` (venv, scratch data dir, isolated ports).
Everything below was actually executed against the fork binary
`rethinkdb 2.4.5-375-gbaa407-dirty (GCC 15.2.0)`.

## 1. Getting started (verified, ~4 min to first success)

```bash
# 1. Start the server (data dir must be created first)
./build/release/rethinkdb create -d /tmp/rt-data
./build/release/rethinkdb serve -d /tmp/rt-data \
    --driver-port 28017 --cluster-port 29017 --http-port 18083 --bind all

# 2. Install the vendored driver in a clean venv
python3 -m venv venv && ./venv/bin/pip install /path/to/rethinkdb/driver/python3
# → installs rethinkdb 2.4.10.post1+source + six, protobuf, looseversion

# 3. Connect
from rethinkdb import r
conn = r.connect(host='127.0.0.1', port=28017)
```

**Host pitfall (this machine):** ports 8080–8083 are occupied by unrelated
services. `rethinkdb serve` FAILS with
`error: Could not bind to http port: The address at localhost:808X is reserved
or already in use.` — it exits rather than picking another port. Always pass
`--http-port` (README documents this; the default 8080 collision is a known
host condition, not a fork bug). Web admin console verified: HTTP 200 + title
"RethinkDB Administration Console".

## 2. What works (verified live)

### Core DB + realtime (the upstream promise holds)
- CRUD: `db_create/table_create/insert/update/delete/filter/count` — all fine.
- **Changefeeds push in realtime**: `table.changes()` delivered insert/update/
  delete events with `old_val`/`new_val` within ~1s of a write from a second
  connection. This is the realtime core and it works.

### PHASE3: time-series (docs/time-series.md is accurate for TS-1..TS-3)
```python
r.db('dogfood').table_create('sensor_readings', primary_key='id', timeSeries={
    'field': 'timestamp', 'chunk_interval': 3600, 'retention': 0}).run(conn)
# config() exposes time_series: {field, chunk_interval, retention, downsample}
```
- 289 docs with `r.epoch_time(...)` time fields inserted cleanly.
- `between(epoch_a, epoch_b, index='timestamp')` returns the exact window
  (24/24 docs for a 2h window at 5-min cadence) — chunk pruning works.
- Errors: missing time field → `TIME_SERIES_FIELD_MISSING` reported in the
  insert RESULT (`{'errors': 1, 'first_error': '...'}` — ReQL write semantics,
  NOT a raised exception); retention > 365d → `TIME_SERIES_RETENTION_EXCEEDED`
  raised at tableCreate.
- **Gotcha:** single-doc insert validation failures come back in the result
  object, not as exceptions — check `res['errors']` or you silently lose docs.

### PHASE3: vector search
```python
t.index_create('vec', vector={'dim': 3, 'metric': 'l2'}).run(conn)
t.index_wait('vec').run(conn)
t.insert({'id': 1, 'vec': r.vector([1.0, 0.0, 0.0])}).run(conn)
list(t.vector_near('vec', r.vector([1.0, 0.1, 0.0]), k=3).run(conn))
# → [{'dist': 0.010000000000000002, 'doc': {...}}, ...]  — dist/doc NESTED shape
```
- Distances are correct L2 (0.01, 0.81, 1.81 for the probe set). Vectors
  round-trip as plain lists without numpy.
- **Gotcha:** `vector_near` returns `{dist, doc}` — the matched document is
  nested under `doc`, not at top level. Not documented anywhere.

### PHASE3: FTS
```python
# REQUIRED form — multi=True is mandatory, docs omit it:
t.index_create('desc', r.row['desc'], fts=True, multi=True).run(conn)
t.index_wait('desc').run(conn)
list(t.fts_match('fruit', index='desc').run(conn))  # → matching docs
```
- Stemming works (`apple` matches `appl`). `r.fts_tokenize('Hello, world!')`
  → `['hello', 'world']`.
- **Gotcha:** with `fts=True` but WITHOUT `multi=True` the index builds and
  reports `ready: true` yet matches NOTHING, silently. This burned the
  dogfood run (see RT-GAP-012).

## 3. What does NOT work (verified live)

### PHASE3: CDC streaming — UNWIRED (see RT-GAP-010)
```python
t.publication_create(name='pub1', filter={}).run(conn)
# → {'created': 1, 'state': 'creating'}   ← stays 'creating' FOREVER
t.subscription_create(name='sub1', publication='pub1', targetTable='replica').run(conn)
# → {'created': 1, 'state': 'creating'}   ← stays 'creating' FOREVER
```
- `publication_status`/`subscription_status` always report `creating` (60s+
  observed). Rows written to the source table NEVER appear in the target table.
- `cdc_sink_create` also stays `creating`; sinks never stream.
- No error message anywhere — the feature looks alive in the API surface but
  no code path transitions the state machines (`READY`/`STREAMING` are never
  assigned; no coroutine/pump exists in the CDC term implementations).
- **Implication:** any app relying on CDC publication/subscription/sink for
  replication or streaming will silently get nothing. Do NOT build on this
  until RT-GAP-010 is fixed or the feature is explicitly marked experimental.
  Plain `changes()` changefeeds are unaffected and work.

## 4. Driver API drift (see RT-GAP-011)

The vendored driver's `Cursor.next()` signature is `next(self, wait=True)` —
it does NOT accept the upstream `timeout=` kwarg. Code written against the
official driver docs (`cursor.next(timeout=5)`) raises
`TypeError: Cursor.next() got an unexpected keyword argument 'timeout'`.
Use `feed.next(wait=2)` (a number of seconds) instead.

## 5. Durability / trust

Kill -TERM the server and restart: all data, tables, time-series configs, and
indexes survived (289 sensor rows, vector + FTS indexes, todos). No corruption,
no recovery errors. Trustworthy at the storage layer.

## 6. What a new user needs that isn't documented

1. CDC actually does nothing yet (biggest one — API exists, pipeline doesn't).
2. `multi=True` requirement for FTS indexes.
3. `vector_near` result shape `{dist, doc}`.
4. `Cursor.next(timeout=)` not supported (use `wait=`).
5. Port collisions on shared hosts → always pass explicit `--http-port`
   (and ideally `--driver-port`/`--cluster-port`).
6. No docs exist for CDC/vector/FTS usage beyond the driver README term table.
