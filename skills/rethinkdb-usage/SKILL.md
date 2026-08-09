---
name: rethinkdb-usage
description: >-
  How to actually USE this RethinkDB fork (v3 protocol, PHASE3 extensions):
  server lifecycle, Python driver install, time-series tables, vector search,
  FTS, changefeeds, and the CDC caveat. Load this skill when landing in this
  repo or when asked to build something on this database. Distilled from the
  2026-08-09 dogfood run (docs/dogfood/2026-08-09-integration.md).
version: 1.0.0
category: database
---

# RethinkDB Fork — Usage Skill

What this project is: a RethinkDB fork (C++17) adding PHASE3 extensions —
time-series storage (chunked, pruned `between()`), vector search
(`vector_near`), FTS (`fts_match`), and CDC publications/subscriptions/sinks.
Core RethinkDB (JSON docs, realtime changefeeds) works as upstream.

## Entry points

- Server binary: `build/release/rethinkdb` (also `make -j4` builds
  `build/release/rethinkdb`).
- Python driver (vendored, v3 protocol): `driver/python3` — pip-installable.
- ReQL docs: `docs/time-series.md` (time-series only). Driver term table:
  `driver/python3/README.md`.

## Run it (verified commands)

```bash
# init + start (data dir required; ALWAYS pick free ports on shared hosts —
# 8080/8081/8082/8083 are taken here and serve() exits on bind failure)
./build/release/rethinkdb create -d /tmp/rt-data
./build/release/rethinkdb serve -d /tmp/rt-data \
  --driver-port 28017 --cluster-port 29017 --http-port 18083 --bind all

# driver in a clean venv (works — RT-GAP-002 fix landed)
python3 -m venv venv && ./venv/bin/pip install <repo>/driver/python3
./venv/bin/python -c "from rethinkdb import r; c = r.connect(host='127.0.0.1', port=28017)"
```

Web admin console: `http://<host>:<http-port>` — verify the page `<title>` is
"RethinkDB Administration Console" (other services squat on 808x here).

## The right way (patterns that work)

- **CRUD:** standard ReQL. Write errors for single-doc validation failures
  come back in the result object (`res['errors']`, `res['first_error']`) —
  NOT as exceptions. Check them or you silently lose writes.
- **Realtime:** `table.changes()` pushes insert/update/delete live. Consume
  with `feed.next(wait=2)` — the vendored driver's `next()` has NO `timeout=`
  kwarg (upstream compat gap, RT-GAP-011).
- **Time-series:**
  ```python
  r.db('d').table_create('t', timeSeries={'field': 'ts', 'chunk_interval': 3600, 'retention': 0})
  # inserts need ts as a TIME pseudo-type (r.epoch_time / r.now / r.iso8601)
  r.db('d').table('t').between(r.epoch_time(a), r.epoch_time(b), index='ts')  # pruned
  ```
  Error codes: TIME_SERIES_FIELD_MISSING (in insert result), TIME_SERIES_RETENTION_EXCEEDED (raised).
- **Vector:**
  ```python
  t.index_create('vec', vector={'dim': 3, 'metric': 'l2'}); t.index_wait('vec')
  t.insert({'id': 1, 'vec': r.vector([1.0, 0.0, 0.0])})
  t.vector_near('vec', r.vector([1.0, 0.1, 0.0]), k=3)
  # → [{'dist': ..., 'doc': {...}}] — doc is NESTED under 'doc'
  ```
- **FTS — multi=True is MANDATORY:**
  ```python
  t.index_create('desc', r.row['desc'], fts=True, multi=True)  # without multi: silently matches NOTHING
  t.fts_match('fruit', index='desc')  # stemming works ('apple'→'appl')
  ```

## Pitfalls (each cost real time on 2026-08-09)

1. **CDC is metadata-only (RT-GAP-010, P0):** publication/subscription/sink
   create/list/status/drop work, but everything stays `state: 'creating'`
   forever and rows never flow to subscription targets or sinks. No error is
   raised. Do NOT build on CDC until the state machine exists.
2. `Cursor.next(timeout=)` → TypeError; use `next(wait=seconds)`.
3. FTS without `multi=True` → ready index, zero matches, no error.
4. `vector_near` results are `{dist, doc}` — read `row['doc']`.
5. Port collisions on this host → explicit `--http-port`/`--driver-port`/
   `--cluster-port`, and verify the web page title before trusting HTTP 200.
6. Docs drift: docs/time-series.md roadmap claims TS-5/TS-6 queued but both
   are committed (RT-GAP-013); there are no CDC/vector/FTS docs beyond the
   driver README term table.

## Verifying a change

- Unit: `make unit` (or run single suites). Integration: the Python probes
  under `test/` (e.g. `driver_e2e_test.py`) need a live server + the venv
  driver. The fastest full-stack check is the dogfood probe pattern:
  start server → pip install driver → CRUD + changes() + time-series
  between() + vector_near + fts_match → restart → data survives
  (scripts: /tmp/dogfood-rethinkdb/probe*.py from the 2026-08-09 run;
  recipe in docs/dogfood/2026-08-09-integration.md).
- NOTE: `test/cdc_e2e_test.py`'s "delivery" test only reads the SOURCE table —
  a green CDC suite does NOT mean rows reach the target (RT-GAP-010).
