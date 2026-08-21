# RethinkDB Dogfood Log

| Date | Verdict | Promise | Top findings | Time-to-first-success |
|------|---------|---------|--------------|----------------------|
| 2026-08-09 | 🟡 PROMISING-BUT-ROUGH | "Open-source realtime NoSQL DB with PHASE3 extensions: CDC streaming, vector search, FTS, time-series" | 1. CDC streaming pipeline unwired (pub/sub/sink stuck 'creating' forever, no delivery; tests don't test delivery) 2. Driver API drift (Cursor.next lacks timeout kwarg) 3. FTS silently empty without multi=True | ~4 min (pip install + server + first CRUD) |
| 2026-08-21 | ✅ SHIPPABLE | "Python user can install driver, create+serve, CRUD + realtime changefeeds + PHASE3 extensions incl. CDC streaming delivery" | 1. CDC streaming NOW DELIVERS end-to-end (RT-GAP-015 verified: pub→sub→target, insert/update/delete; was the 08-09 P0) 2. doc examples reject (conflict:'replace', optarg names, JS-only generated-columns; merge_deep table trap) 3. observability gaps (subscription status 'creating' while flowing; sinks silent stubs) | ~2 min (create + serve + connect + UI) |

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

## 2026-08-21 — Second dogfood run (cron, target: rethinkdb)

**Promise statement:** A Python user can `pip install ./driver/python3`, run
`rethinkdb create` + `serve` per the README quickstart, store schemaless JSON
docs, receive realtime changefeed pushes, and use the PHASE3 extensions —
CDC publication→subscription→target streaming, time-series, vector search,
FTS, UPSERT/MERGE_DEEP, generated columns, partitioning.

**How it was used (real consumer in /tmp/dogfood-r2, fresh venv, scratch
data dir, ports 28019/29019/18085):**
- Probe 1 (CDC headline): publication_create → state `ready`; subscription →
  rows inserted into source appeared in target table 5/5 within seconds;
  update AND delete propagated too. RT-GAP-015 genuinely fixed. (08-09 P0.)
- Probe 2: CRUD, changefeeds (single-thread + concurrent separate-connection
  patterns), UPSERT merge semantics, MERGE_DEEP datum-level (deep=True,
  element-wise arrays), generated columns (lambda form), partitioning (list
  type, partition_info, filtered routing), vector_near L2 ordering, FTS
  multi=True, time-series between() pruning — ALL PASS.
- Probe 3: durability — SIGTERM + restart; all tables and rows intact.
- Probe 4: security defaults — no `--bind` → loopback-only + empty-admin
  warning (RT-GAP-033 PASS criteria met); `--bind all` + no password → empty
  admin reachable (premise narrowed).
- Sink check: file sink = silent stub (no output, no error) — documented.
- Web admin UI: HTTP 200, "RethinkDB Administration Console" title.

**Top 3 findings (task IDs RT-GAP-036..042):**
1. RT-GAP-036: docs/cdc-streaming.md `conflict: "replace"` rejected by server
   (valid: last_write_wins/primary_key_merge/custom) — doc example fails.
2. RT-GAP-039 (P1): merge_deep exposed on Table but datum-level only —
   confusing `Expected type DATUM but found TABLE` trap.
3. RT-GAP-037/038: CDC observability — subscription status `creating` while
   delivering; sinks silent no-ops. Plus RT-GAP-040 (driver README omits fork
   terms), RT-GAP-041 (generated-columns doc JS-only), RT-GAP-042 (RT-GAP-033
   premise drift).

**Friction count:** 7 (3 doc-example traps, 1 driver API trap, 2 observability
gaps, 1 cleared false alarm — shared-connection threading misuse; all
recovered in minutes, error messages name the right alternatives).

**Verdict:** ✅ SHIPPABLE. Core DB + realtime + ALL PHASE3 features work
through the official driver; CDC — the 08-09 promise-breaker — now delivers.
Remaining issues are docs/UX polish with clear error messages, no functional
blockers. Foreman NOT woken: cooldown 21600 is the documented operator pin
(gap-hunter cycles note "wake SKIP"); scheduler health verified (namespace
coding-hermes live, decay 1, tick #179 completed/committed).
