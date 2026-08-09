# RethinkDB Dogfood Log

| Date | Verdict | Promise | Top findings | Time-to-first-success |
|------|---------|---------|--------------|----------------------|
| 2026-08-09 | 🟡 PROMISING-BUT-ROUGH | "Open-source realtime NoSQL DB with PHASE3 extensions: CDC streaming, vector search, FTS, time-series" | 1. CDC streaming pipeline unwired (pub/sub/sink stuck 'creating' forever, no delivery; tests don't test delivery) 2. Driver API drift (Cursor.next lacks timeout kwarg) 3. FTS silently empty without multi=True | ~4 min (pip install + server + first CRUD) |

## 2026-08-09 — First dogfood run (cron, target: rethinkdb)

**Promise statement:** A Python user can `pip install ./driver/python3`, start the fork server, store schemaless JSON docs, subscribe to realtime changefeeds, and use the PHASE3 extensions (time-series tables with chunk pruning, vector similarity search, FTS, CDC publication/subscription/sink streaming) — per README + docs/time-series.md + driver/python3/README.md.

**How it was used (real consumer in /tmp/dogfood-rethinkdb, venv, scratch data dir, ports 28017/29017/18083):**
- Probe 1: CRUD (create db/table, insert 3, update, delete, filter) — PASS
- Probe 2: changefeed push (insert/update/delete delivered live to `changes()` cursor) — PASS
- Probe 3: time-series (tableCreate timeSeries optarg, config() exposure, 289 inserts, between() window = 24/24, TIME_SERIES_FIELD_MISSING in write result, TIME_SERIES_RETENTION_EXCEEDED raised) — PASS
- Probe 4: vector (index_create vector optarg, vector_near correct L2 ordering/distances, result shape {dist, doc}) + FTS (fts_match with multi=True, stemming works) — PASS
- Probe 5: CDC (publication/subscription/sink create/list/status work as metadata CRUD, but state stays `creating` forever; target table never receives rows) — **FAIL (feature unwired)**
- Probe 6: durability (kill -TERM server, restart, all tables/indexes/data intact) — PASS
- pip install ./driver/python3 in clean venv — PASS (RT-GAP-002 premise stale)
- Web admin console serves at --http-port (title "RethinkDB Administration Console") — PASS

**Top 3 findings (task IDs):**
1. RT-GAP-010 (P0): CDC streaming pipeline is a false-complete — no state machine, no delivery, tests don't test delivery.
2. RT-GAP-011 (P1): vendored driver Cursor.next() lacks `timeout` kwarg (upstream compat break).
3. RT-GAP-012 (P1): FTS index silently matches nothing unless `multi=True` (undocumented).

**Friction count:** 6 (2x port collision on crowded host — documented README workaround; Cursor.next(timeout=) TypeError; FTS empty-result trap; vector_near result shape {dist,doc} undocumented; CDC 'creating' forever with zero diagnostics; board says pip-install broken but it's fixed).

**Verdict:** 🟡 PROMISING-BUT-ROUGH. Core DB + realtime changefeeds + time-series + vector + FTS genuinely work and are usable. The flagship CDC streaming does not deliver (metadata CRUD only). Value is real; the CDC gap is a hard promise-breaker for the fork's headline feature.
