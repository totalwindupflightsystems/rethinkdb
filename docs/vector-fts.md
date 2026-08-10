# RethinkDB Vector / FTS / BRIN Indexes

**Status: implemented.** This fork adds three index families on top of the
standard secondary index: vector similarity search, full-text search (FTS),
and BRIN block-range indexes. All are created with `indexCreate` and queried
with dedicated terms. Covered by integration tests (see
[Evidence](#evidence)).

## Overview

| Index family | Index optargs | Query term |
|--------------|---------------|------------|
| Vector | `vector: {dim, metric}` | `vectorNear` (term `vector_near`) |
| FTS | `fts: true, multi: true` | `ftsMatch` / `match()` |
| BRIN | `brin: {columns, range_size}` | `between(...)` with `index:` |

## Creating a vector index

`indexCreate` with a `vector` optarg makes the field's array-of-numbers a
nearest-neighbor index. The vector must have a fixed dimension and one of
three metrics:

```javascript
r.db("catalog").table("items").indexCreate("vec_l2_idx", r.row("vec_attr"), {
  vector: { dim: 3, metric: "l2" }            // Euclidean distance
})

r.db("catalog").table("items").indexCreate("vec_cos_idx", r.row("vec_attr"), {
  vector: { dim: 3, metric: "cosine" }        // cosine similarity
})

r.db("catalog").table("items").indexCreate("vec_ip_idx", r.row("vec_attr"), {
  vector: { dim: 3, metric: "inner_product" } // dot product
})
```

`metric` values: `l2`, `cosine`, `inner_product`. Indexes must be created
sequentially (one at a time, waiting for readiness) — concurrent creation can
contend on the superblock and hang `indexWait`; use `indexWait(name)` before
creating the next one.

## Querying a vector index

`vectorNear(indexName, queryVector, {k})` returns the `k` nearest documents
(default `k = 10`, max 10000). The query vector comes from `r.vector()`:

```javascript
r.db("catalog").table("items").vectorNear(
  "vec_l2_idx",
  r.vector([0.1, 0.2, 0.3]),      // also accepts a plain array
  { k: 5 }
)
```

## Creating an FTS index

```javascript
r.db("catalog").table("items").indexCreate("fts_content_idx", r.row("content"), {
  fts: true,
  multi: true
})
```

## Querying FTS

Two ways to run full-text queries:

```javascript
// Table-level ftsMatch with an explicit index
r.db("catalog").table("items").ftsMatch("hello world", { index: "fts_content_idx" })

// filter with .match() on the indexed field (uses the FTS index)
r.db("catalog").table("items").filter(r.row("content").match("hello"))
r.db("catalog").table("items").filter(r.row("name").match("item"))
```

A term that appears in no document matches nothing (`match("zzzznonexistent")`
returns zero rows).

## Creating a BRIN index

BRIN (Block Range INdex) indexes summarize ranges of blocks for a column
instead of indexing every value — cheap to build, ideal for large,
physically-ordered datasets (e.g. time-series columns):

```javascript
r.db("catalog").table("items").indexCreate("brin_score_idx", r.row("score"), {
  brin: { columns: ["score"], range_size: 128 }
})

// range_size is optional (server default applies)
r.db("catalog").table("items").indexCreate("brin_ts_idx", r.row("timestamp"), {
  brin: { columns: ["timestamp"] }
})
```

## Querying through a BRIN index

Use `between` with the BRIN index name:

```javascript
r.db("catalog").table("items").between(0, 60, { index: "brin_score_idx" })
```

## Index metadata and readiness

```javascript
r.db("catalog").table("items").indexList()
// → ["vec_l2_idx", "vec_cos_idx", "vec_ip_idx", "brin_score_idx", "brin_ts_idx", "fts_content_idx"]

r.db("catalog").table("items").indexWait()
// → [{ index: "vec_l2_idx", ready: true, ... }, ...]

r.db("catalog").table("items").indexStatus("vec_l2_idx")
// → [{ index: "vec_l2_idx", ready: true, vector: true, vector_metric: "l2", ... }]
```

Vector index status reports `vector: true` and `vector_metric`; some older
driver versions may not surface those fields in the status response.

## Notes

- Primary-key `between` and `get` work unchanged alongside these indexes.
- `filter` on plain fields remains a full scan; use a BRIN index when the
  predicate is a range over an ordered column.
- `fts_tokenize` is also available for tokenization without querying.

## Evidence

- `test/vector_fts_integration_test.py` — L2/cosine/inner_product vector
  indexes, FTS index with `multi: true`, `match()` queries, `indexList` /
  `indexWait` / `indexStatus`, and edge cases.
- `test/vector_fts_brin_integration_test.py` — vector indexes plus BRIN
  creation (explicit and default `range_size`), BRIN `between` queries, FTS
  creation, and the 6-index metadata suite.
- Terms: `src/rdb_protocol/terms/vector.cc`, `src/rdb_protocol/terms/vector_near.cc`,
  `src/rdb_protocol/terms/fts.cc`.
