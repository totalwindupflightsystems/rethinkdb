---
name: rethinkdb-usage
description: >-
  How to actually USE this RethinkDB fork (v3 protocol, PHASE3 extensions):
  server lifecycle, Python driver install, time-series tables, vector search,
  FTS, changefeeds, CDC streaming (publication/subscription), UPSERT/
  MERGE_DEEP, generated columns, partitioning. Load this skill when landing in
  this repo or when asked to build something on this database. Distilled from
  the 2026-08-09 + 2026-08-21 dogfood runs
  (docs/dogfood/2026-08-21-integration.md).
version: 1.1.0
category: database
---

# RethinkDB Fork — Usage Skill

What this project is: a RethinkDB fork (C++17) adding PHASE3 extensions —
CDC publications/subscriptions/sinks, time-series storage (chunked, pruned
`between()`), vector search (`vector_near`), FTS (`fts_match`),
UPSERT/MERGE_DEEP, generated columns, table partitioning. Core RethinkDB
(JSON docs, realtime changefeeds) works as upstream.

**Status as of 2026-08-21 dogfood run: ✅ SHIPPABLE.** CDC streaming now
delivers end-to-end (RT-GAP-015 fixed; previously stuck `creating` forever).
Known honest limitations: sink drivers are stubs (metadata only), subscription
status may report `creating` while rows flow, snapshot mode is `none`.

## Entry points

- Server binary: `build/release/rethinkdb` (`make -j4` also builds it).
- Python driver (vendored, v3 protocol): `driver/python3` — pip-installable.
- Feature docs: `docs/{time-series,vector-fts,cdc-streaming,merge-upsert,
  generated-columns,partitioning}.md` — NOTE: driver/python3/README.md does
  NOT link them (RT-GAP-040).

## Run it (verified commands)

```bash
# init + start — data dir REQUIRED (RT-GAP-032: create before serve)
./build/release/rethinkdb create -d /tmp/rt-data
./build/release/rethinkdb serve -d /tmp/rt-data \
  --driver-port 28019 --cluster-port 29019 --http-port 18085 --bind all
# ports 8080-8082 are taken by other services on this host — always pick
# explicit ports; default serve (no --bind) is loopback-only + warns about
# the empty admin password (RT-GAP-033: exposure needs explicit --bind all)

# driver in a clean venv
python3 -m venv venv && ./venv/bin/pip install <repo>/driver/python3
./venv/bin/python -c "from rethinkdb import r; c = r.connect(host='127.0.0.1', port=28019)"
```

Web admin console: `http://<host>:<http-port>` — verify `<title>` is
"RethinkDB Administration Console" (other services squat on 808x here).

## The right way (patterns that work)

- **CRUD:** standard ReQL. Single-doc validation failures come back in the
  result object (`res['errors']`, `res['first_error']`) NOT as exceptions.
- **Realtime:** `table.changes()` pushes insert/update/delete live.
  ⚠️ **One connection per thread** — the driver is NOT thread-safe; sharing a
  connection across threads corrupts framing (`JSONDecodeError: Extra data`).
  Writer connection + consumer connection is the verified pattern.
- **CDC streaming (VERIFIED working):**
  ```python
  t.publication_create({'name': 'pub', 'format': 'json', 'snapshot': 'none'})
  t.subscription_create({'name': 'sub', 'publication': 'pub',
      'target': {'db': 'd', 'table': 'target'},
      'conflict': 'last_write_wins', 'snapshot': 'none'})
  # insert into source -> rows appear in target within seconds
  ```
  - `conflict` accepts `last_write_wins` | `primary_key_merge` | `custom` —
    `'replace'` (docs example) is REJECTED by the server (RT-GAP-036).
  - Publication status reaches `ready`; subscription status may stay
    `creating` even while rows flow (RT-GAP-037) — verify delivery by
    counting the target table, not by status.
  - Sinks (`cdcSinkCreate`) are STUBS: create returns `{created, state:
    'creating'}` and nothing ever delivers or errors (RT-GAP-038). Use
    subscriptions for real streaming.
- **UPSERT (216):** `t.upsert(doc)` — merges into existing doc (conflict=
  update default), inserts when missing. Real table write.
- **MERGE_DEEP (217):** DATUM-LEVEL ONLY. `r.expr(base).merge_deep(over,
  deep=True)` (element-wise array merge when deep=True; shallow replaces
  arrays). `t.merge_deep(...)` fails confusingly — do NOT call it on a table
  (RT-GAP-039).
- **Generated columns (218/219):** post-create call, Python lambda form:
  ```python
  t.set_generated_columns({'total': lambda row: row['price'] * row['quantity']})
  t.get_generated_columns()   # read back config
  ```
  `r.row[...]` values FAIL in Python (`r.row is not defined in this context`);
  `table_create(generated_columns=...)` optarg is rejected (RT-GAP-041).
- **Partitioning:** `table_create('t', partitions={'by': 'region',
  'type': 'list', 'partitions': [{'name': 'us', 'values': ['us']},
  {'name': 'other', 'default': True}]})` — optarg is `partitions` (not
  `partitioning`); types range/hash/list per docs/partitioning.md;
  `t.partition_info()` + `t.repartition(...)`.
- **Time-series:** `table_create('t', timeSeries={'field': 'ts',
  'chunk_interval': 3600, 'retention': 0})`; inserts need ts as TIME
  pseudo-type (`r.epoch_time`); `between(a, b, index='ts')` prunes by chunk.
  Error codes: TIME_SERIES_FIELD_MISSING (in result),
  TIME_SERIES_RETENTION_EXCEEDED (raised).
- **Vector:** `t.index_create('vec', vector={'dim': 3, 'metric': 'l2'});
  t.index_wait('vec'); t.insert({'id': 1, 'vec': r.vector([1.0, 0, 0])});
  t.vector_near('vec', r.vector([1.0, 0.1, 0]), k=3)` → `[{'dist': ...,
  'doc': {...}}]` (doc NESTED).
- **FTS — multi=True is MANDATORY:** `index_create('desc', r.row['desc'],
  fts=True, multi=True)`; without multi it silently matches nothing.
  `t.fts_match('fruit', index='desc')`.

## Pitfalls (each cost real time on 2026-08-09/21)

1. CDC `conflict: 'replace'` → server error; use `last_write_wins`.
2. `Cursor.next(timeout=)` → TypeError; use `next(wait=seconds)` (RT-GAP-011).
3. FTS without `multi=True` → ready index, zero matches, no error.
4. `vector_near` results are `{dist, doc}` — read `row['doc']`.
5. Port collisions on this host → explicit ports; verify the web page title.
6. `merge_deep` on a table → confusing DATUM error; it's datum-level.
7. `r.row` in set_generated_columns → error; use Python lambdas.
8. Shared connection across threads → corrupted framing; one conn per thread.
9. Driver README term table omits the fork terms — use docs/*.md instead.

## Verifying a change

- Unit: `make unit`. Integration: Python probes under `test/` (script mode,
  spawn `build/release/rethinkdb`; needs `rethinkdb create` first — RT-GAP-031).
- Fastest full-stack check (dogfood pattern): start server → pip install
  driver → CRUD + changes() + CDC pub/sub delivery (count TARGET table) +
  time-series between() + vector_near + fts_match → restart → data survives.
  Recipes in `docs/dogfood/2026-08-21-integration.md` (probes in
  /tmp/dogfood-r2/probe_*.py).
- Board truth lives in `.coding-hermes/board/tasks.jsonl` (tasks.md is a
  frozen legacy log — do not dispatch from it).
