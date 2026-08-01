# RethinkDB Python Driver (v3 — vendored, upgraded)

This is the official `rethinkdb` Python driver, regenerated and upgraded to
speak the v3 protocol of this fork. The bundled `ql2_pb2.py` is generated
from `src/rdb_protocol/ql2.proto` and includes **all 18 new ReQL terms**
(IDs 198–215):

| # | Term | Python API |
|---|------|-----------|
| 198 | `FTS_TOKENIZE` | `r.fts_tokenize(text)` |
| 199 | `FTS_MATCH` | `table.fts_match(query, index=...)` |
| 200 | `VECTOR` | `r.vector([...])` |
| 201 | `VECTOR_NEAR` | `table.vector_near(index, r.vector([...]), k=N)` |
| 202 | `PARTITION_INFO` | `table.partition_info()` |
| 203 | `REPARTITION` | `table.repartition(...)` |
| 204–207 | CDC publications | `table.publication_create/list/status/drop` |
| 208–211 | CDC subscriptions | `table.subscription_create/list/status/drop` |
| 212–215 | CDC sinks | `table.cdc_sink_create/list/status/drop` |

## Install

```bash
pip install ./driver/python3
```

or use directly without installing:

```bash
PYTHONPATH=driver/python3 python3 your_app.py
```

## Notes

- Vector datums round-trip as numpy arrays (or plain lists if numpy is
  absent) via the `VECTOR` pseudo-type.
- CDC configs accept keyword arguments: `publication_create(name='pub1',
  filter={})` is equivalent to passing a config object positionally.
- The vendored `ast.py`/`query.py` add query-builder methods for all new
  terms; the stock upstream driver does not know them.
