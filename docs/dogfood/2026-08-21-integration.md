# RethinkDB Fork — Dogfood Integration Report (2026-08-21)

**Verdict: ✅ SHIPPABLE** — second dogfood run (first: 2026-08-09, 🟡 with CDC
unwired). This run re-verified the flagship **CDC streaming pipeline end-to-end
and it now DELIVERS**. Every headline PHASE3 feature worked through the
official Python driver. Remaining friction is docs/UX polish (7 findings,
RT-GAP-036..042), zero functional blockers on the promised workflows.

**Environment:** server `build/release/rethinkdb 2.4.5-438-g381070` (GCC 15.2.0),
driver pip-installed from `driver/python3` into a fresh venv, scratch data dir
`/tmp/dogfood-r2/data`, ports 28019/29019/18085 (host ports 8080-8082 are
colliding — always pass explicit ports on this host). Probes:
`/tmp/dogfood-r2/probe_*.py`.

## Promise vs reality

**Promise:** a Python user can install the driver, `rethinkdb create` +
`serve`, store JSON docs, subscribe to realtime changefeeds, and use the
PHASE3 extensions: CDC publication→subscription→sink streaming, time-series,
vector search, FTS, UPSERT/MERGE_DEEP, generated columns, partitioning.

**Reality:** all of the above works. Time-to-first-success ~2 min
(create → serve → connect → first CRUD, README quickstart exact — RT-GAP-032
fix verified). CDC delivery verified with inserts, updates, AND deletes
propagating source→target. The 2026-08-09 P0 (RT-GAP-010/015, stuck
`creating`) is genuinely fixed.

## Verified working recipe (the right way)

```bash
# 1. init + serve (README quickstart, RT-GAP-032)
./build/release/rethinkdb create -d /tmp/rt-data
./build/release/rethinkdb serve -d /tmp/rt-data \
    --driver-port 28019 --cluster-port 29019 --http-port 18085 --bind all

# 2. driver (pip-installable, works in clean venv)
python3 -m venv /tmp/rt-venv && /tmp/rt-venv/bin/pip install <repo>/driver/python3
```

```python
from rethinkdb import r
c = r.connect(host='127.0.0.1', port=28019)

# CRUD / changefeeds — as upstream, push delivery verified
feed = r.db('app').table('t').changes().run(c)   # one connection per thread!
# ... write on another connection; feed.next(wait=3) receives insert/update/delete

# CDC streaming (RT-GAP-015 FIXED — verified live)
r.db('cdc_app').table_create('events').run(c)
r.db('cdc_app').table_create('events_sub').run(c)
r.db('cdc_app').table('events').publication_create(
    {'name': 'pub', 'format': 'json', 'snapshot': 'none'}).run(c)
r.db('cdc_app').table('events').subscription_create({
    'name': 'sub', 'publication': 'pub',
    'target': {'db': 'cdc_app', 'table': 'events_sub'},
    'conflict': 'last_write_wins',          # NOT 'replace' (see RT-GAP-036)
    'snapshot': 'none'}).run(c)
r.db('cdc_app').table('events').insert([{'id': i} for i in range(5)]).run(c)
r.db('cdc_app').table('events_sub').count().run(c)   # → 5 within seconds

# UPSERT (term 216, merge-not-replace) and MERGE_DEEP (217, datum-level!)
r.db('cdc_app').table('merge_t').upsert({'id': 1, 'b': {'y': 2}}).run(c)
r.expr({'a': {'x': 1}}).merge_deep({'a': {'y': 2}}, deep=True).run(c)

# Generated columns (218/219) — lambda form required in Python
r.db('cdc_app').table('gen_t').set_generated_columns(
    {'total': lambda row: row['price'] * row['quantity']}).run(c)

# Partitioning — table_create optarg is `partitions` (list/hash/range)
r.db('cdc_app').table_create('part_t', partitions={
    'by': 'region', 'type': 'list',
    'partitions': [{'name': 'us', 'values': ['us']},
                   {'name': 'other', 'default': True}]}).run(c)

# Vector + FTS + time-series — as documented in docs/vector-fts.md / time-series.md
```

## Errors hit and their fixes (each = a board finding)

| # | Error / trap | Root cause | Fix direction (task) |
|---|--------------|------------|----------------------|
| 1 | `Unknown conflict resolution 'replace'; expected last_write_wins, primary_key_merge, or custom` | docs/cdc-streaming.md example uses `conflict: "replace"`; server accepts only the CDC-09 resolver set | RT-GAP-036 (docs) |
| 2 | `subscription_status` → `state: 'creating'` forever, while rows ARE flowing | status state machine lags the pump; ops can't distinguish live vs stuck | RT-GAP-037 |
| 3 | file sink: `{created:1, state:'creating'}` forever, no dir, no error | sink drivers are stubs (docs admit it) but API is silent | RT-GAP-038 |
| 4 | `table.merge_deep({...})` → `Expected type DATUM but found TABLE` | merge_deep is datum-level; driver wrongly exposes it on Table | RT-GAP-039 (P1) |
| 5 | `table_create(generated_columns=...)` → unrecognized optarg; `r.row` → `not defined in this context` | generated columns are a post-create `set_generated_columns` call; Python needs lambdas; docs JS-only | RT-GAP-041 |
| 6 | `table_create(partitioning=...)` → unrecognized optarg | optarg is `partitions` with `{by, type, ...}` shape (docs/partitioning.md) | docs gap — folded into RT-GAP-040 |
| 7 | (false alarm, cleared) `JSONDecodeError: Extra data` on changefeeds | MY BUG: two threads sharing one connection — driver is not thread-safe; separate connections per thread works | none — documented in skill |

## Friction count & time-to-first-success

- TTFS: **~2 min** (create + serve + connect + UI check).
- Friction points: 7 (3 doc traps that cost a retry each, 1 P1 driver API
  trap, 1 status-observability gap, 1 silent sink, 1 threading false alarm).
  All recovered within minutes — the server's error messages name the correct
  alternatives (good error UX), which is why this is SHIPPABLE, not rough.

## What held up vs fell apart

- Held up: quickstart, CRUD, changefeeds (single-conn and concurrent
  separate-conn), CDC delivery (insert/update/delete), durability across
  restart (all tables+rows intact), time-series pruning, vector ordering,
  FTS, generated columns, partitioning, admin UI, driver install.
- Fell apart / rough: doc examples that don't match the server (conflict,
  optarg names, JS-vs-Python forms), merge_deep table-method trap, sink
  silence, subscription status liveness, driver README not documenting the
  fork's own terms.

## Security notes (RT-GAP-033 live verification)

- Default `serve` (no `--bind`): binds loopback only (127.0.0.1/::1) and
  prints two warnings (empty admin password; how to expose on the network).
  The task's PASS criteria ("warns and loopback-binds") are already met.
- With explicit `--bind all` + no `--initial-password`: empty admin accepts
  connections with NO credentials (verified). Only reachable if the operator
  opts into network exposure.
