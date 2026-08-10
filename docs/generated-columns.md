# RethinkDB Generated / Virtual Columns

**Status: implemented.** This fork supports generated (virtual) columns:
columns whose values are computed by a ReQL function over the row at write
time, instead of being stored in the document. The implementation lives in
`src/rdb_protocol/terms/generated_columns.cc` and is applied on the write
path in `src/rdb_protocol/real_table.cc` / `src/rdb_protocol/store.cc`.

## Overview

Generated columns are configured per table as a map of column name → ReQL
function of the row. When a document is written, each generated column's
function runs against the row and the result is merged into the stored
document — so reads see the computed value without the application having to
maintain it.

| Term | Purpose |
|------|---------|
| `setGeneratedColumns({col: func, ...})` (term `SET_GENERATED_COLUMNS`) | Install (or replace) the table's generated columns; empty object drops them |
| `getGeneratedColumns()` (term `GET_GENERATED_COLUMNS`) | Return the current generated-column config |

## Setting generated columns

`setGeneratedColumns` takes an object mapping column names to functions. The
function receives the row and must be deterministic (non-deterministic
functions are rejected at install time).

```javascript
// total = price * (1 + tax_rate), computed at write time
r.db("app").table("orders").setGeneratedColumns({
  total: r.row("price").mul(r.row("tax_rate").add(1)),
  has_tags: r.row("tags").count().gt(0)
})
// → { created: 1 }

// Replace the whole set in one call
r.db("app").table("orders").setGeneratedColumns({
  total: r.row("price").mul(r.row("tax_rate").add(1))
})
// → { created: 1 }

// Drop all generated columns
r.db("app").table("orders").setGeneratedColumns({})
// → { dropped: 1 }
```

Rules (as implemented in `parse_generated_columns_raw`):

- The argument must be an object mapping column names to functions —
  `"Generated columns must be specified as an object mapping column names to
  functions."`
- Column names must be non-empty and unique (duplicates fail with
  `"Duplicate generated column name: <name>."`).
- Each function must be deterministic
  (`"Generated column functions must be deterministic."`) and expect 0 or 1
  argument (the row) — `"Generated column <name> must expect 1 argument (the
  row)."`
- A driver lambda (`function(row) { ... }`) uses its own parameter list; a
  raw expression is bound as a 1-arg function over the row.
- Responses report `{ created: 1 }` when replacing/installing a non-empty
  config and `{ dropped: 1 }` when clearing with `{}`.

## Reading generated columns

```javascript
r.db("app").table("orders").getGeneratedColumns()
// → { total: "r.row(function(row) { return ...; })", has_tags: "..." }
```

`getGeneratedColumns` returns the configured map as column name → the
compiled function formatted as a JavaScript ReQL string.

## Write behavior

Generated columns are applied on every write to the table: inserts,
updates, and replacements merge the computed values into the stored document
before it is written. The functions are compiled once per write batch from
the table's generated-column config (see the write path in
`src/rdb_protocol/store.cc`), so computed values are always consistent with
the rest of the row.

## Evidence

- Source of truth for the terms and their optargs:
  `src/rdb_protocol/terms/generated_columns.cc`
  (`set_generated_columns` / `get_generated_columns`).
- Write-path application: `src/rdb_protocol/real_table.cc`
  (`get_generated_columns`), `src/rdb_protocol/store.cc`
  (`apply_generated_columns`).
- Driver bindings: `driver/python3/rethinkdb/ast.py`
  (`set_generated_columns`, `get_generated_columns`).
- Admin interface: `src/clustering/administration/real_reql_cluster_interface.cc`
  (`set_generated_columns`, table config change with
  `generated_columns_set_t` / `generated_columns_drop_t`).
