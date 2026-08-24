# RethinkDB Python Driver (v3 — vendored, upgraded)

This is the official `rethinkdb` Python driver, regenerated and upgraded to
speak the v3 protocol of this fork. The bundled `ql2_pb2.py` is generated
from `src/rdb_protocol/ql2.proto` and includes **all 22 new ReQL terms**
(IDs 198–219):

| # | Term | Python API |
|---|------|-----------|
| 198 | `FTS_TOKENIZE` | `r.fts_tokenize(text)` |
| 199 | `FTS_MATCH` | `table.fts_match(query, index=...)` (FTS index needs `multi=True` — see below) |
| 200 | `VECTOR` | `r.vector([...])` |
| 201 | `VECTOR_NEAR` | `table.vector_near(index, r.vector([...]), k=N)` |
| 202 | `PARTITION_INFO` | `table.partition_info()` |
| 203 | `REPARTITION` | `table.repartition(...)` |
| 204–207 | CDC publications | `table.publication_create/list/status/drop` |
| 208–211 | CDC subscriptions | `table.subscription_create/list/status/drop` |
| 212–215 | CDC sinks | `table.cdc_sink_create/list/status/drop` |
| 216 | `UPSERT` | `table.upsert(doc)` — insert-or-replace by primary key (see below) |
| 217 | `MERGE_DEEP` | `r.expr(base).merge_deep(over, deep=True)` — datum-level deep merge (see below) |
| 218 | `SET_GENERATED_COLUMNS` | `table.set_generated_columns({'total': lambda row: row['price'] * row['qty']})` — post-create call, not a `table_create` optarg |
| 219 | `GET_GENERATED_COLUMNS` | `table.get_generated_columns()` |

`table_create` also accepts two fork optargs: `timeSeries=True` (chunked
time-ordered storage with retention TTL) and `partitions={...}` (declarative
partition layout).

## FTS indexes

Full-text search requires a `multi=True` index. An index created without it
reports ready, but `fts_match` silently returns `[]` with no error:

```python
table.index_create("fts_content_idx", r.row["content"], fts=True, multi=True)
table.fts_match("hello world", index="fts_content_idx")
```

## Install

```bash
pip install ./driver/python3
```

or use directly without installing:

```bash
PYTHONPATH=driver/python3 python3 your_app.py
```

## Connecting / Quickstart

Open a connection and run a first query (both import styles work; the
module-level `connect` is a convenience alias for `r.connect`):

```python
from rethinkdb import r

conn = r.connect(host='localhost', port=28015)   # default: localhost:28015
r.db_create('quickstart').run(conn)
r.db('quickstart').table_create('messages').run(conn)
r.db('quickstart').table('messages').insert(
    {'id': 1, 'text': 'hello rethinkdb'}
).run(conn)
doc = r.db('quickstart').table('messages').get(1).run(conn)
print(doc)   # {'id': 1, 'text': 'hello rethinkdb'}
conn.close()
```

Equivalent module-level form:

```python
import rethinkdb
conn = rethinkdb.connect(host='localhost', port=28015)
```

Subscribe to a changefeed:

```python
from rethinkdb import r

conn = r.connect()
feed = r.db('quickstart').table('messages').changes().run(conn)
# every insert/update/delete on the table is pushed to the feed
```

## Notes

- Vector datums round-trip as numpy arrays (or plain lists if numpy is
  absent) via the `VECTOR` pseudo-type.
- CDC configs accept keyword arguments: `publication_create(name='pub1',
  filter={})` is equivalent to passing a config object positionally.
- The vendored `ast.py`/`query.py` add query-builder methods for all new
  terms; the stock upstream driver does not know them.
- `Cursor.next()` accepts a bounded-timeout override: `cursor.next(timeout=5)`
  waits at most 5 seconds for the next element (ignoring `wait`), raising
  `ReqlTimeoutError` on expiry. A non-negative number is required; anything
  else raises `ReqlDriverError`. `timeout=0` is equivalent to `wait=False`.
- `merge_deep` is a datum-level operator — call it on a datum/expression
  (`r.expr(base).merge_deep(over, deep=True)`), not on a table. Calling it
  on a table raises `ReqlQueryLogicError`.
- `upsert` is an insert-or-replace by primary key: `table.upsert({'id': 1,
  'count': 2})` inserts the doc or fully replaces the existing row with the
  same primary key (no field-level merge, no conflict option).
- Generated columns are configured with a **post-create** call
  (`table.set_generated_columns({...})`) — passing `generated_columns=` to
  `table_create` is rejected. The mapping values are Python lambdas over the
  row: `table.set_generated_columns({'total': lambda row: row['price'] *
  row['quantity']})`. `get_generated_columns()` returns the installed map.
- `table_create(..., timeSeries=True)` enables chunked time-ordered storage
  (retention TTL via the time-series config) and `table_create(...,
  partitions={...})` installs a declarative partition layout. Both optargs
  are fork extensions.
