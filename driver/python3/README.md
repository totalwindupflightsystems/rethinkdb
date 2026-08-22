# RethinkDB Python Driver (v3 — vendored, upgraded)

This is the official `rethinkdb` Python driver, regenerated and upgraded to
speak the v3 protocol of this fork. The bundled `ql2_pb2.py` is generated
from `src/rdb_protocol/ql2.proto` and includes **all 18 new ReQL terms**
(IDs 198–215):

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
