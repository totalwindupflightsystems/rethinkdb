# RethinkDB MERGE / UPSERT Extension

**Status: implemented.** This fork adds Postgres-style write and merge
semantics: `upsert` (insert-or-update, `ON CONFLICT DO UPDATE` parity) and
`merge_deep` (recursive deep merge that descends into arrays). Covered by an
end-to-end test (see [Evidence](#evidence)).

## Overview

| Term | What it does | Stock equivalent |
|------|--------------|------------------|
| `table.upsert(doc)` | Insert, or update the existing row on primary-key conflict | `insert` with `conflict: "update"` |
| `r.expr(obj).mergeDeep(other, {deep: true})` | Recursive object merge; arrays merge element-wise | `merge` (replaces arrays wholesale) |

## UPSERT

`upsert` behaves like `insert` with `conflict="update"` as the **default**:
the row is inserted when the primary key is absent and merged over the
existing row when it is present. All `insert` optargs still work, and an
explicit `conflict` optarg overrides the default.

```javascript
r.db("app").table("t").upsert({ id: "a", n: 1, tags: ["x"] })
// → { inserted: 1 }

// Same key: updates in place — n becomes 2, tags survives (merged, not replaced)
r.db("app").table("t").upsert({ id: "a", n: 2 })
// → { replaced: 1 }

// New key: inserts
r.db("app").table("t").upsert({ id: "b", n: 5 })
// → { inserted: 1 }

// Explicit conflict optarg still honored (Postgres ON CONFLICT DO NOTHING parity)
r.db("app").table("t").upsert({ id: "a", n: 99 }, { conflict: "error" })
// → { errors: 1, inserted: 0 }
```

Upsert merges the incoming fields over the existing document — fields not in
the new document (like `tags` above) are kept.

## MERGE_DEEP

`mergeDeep` is a superset of the stock `merge`: nested objects recurse
field-by-field, and arrays merge **element-wise by position** instead of
being replaced wholesale. Without the `deep: true` optarg it behaves like
stock `merge` (arrays replaced).

```javascript
const base = {
  a: 1,
  nested: { x: 1, y: 2 },
  arr: [{ p: 1, q: 2 }, { r: 3 }]
};
const over = {
  nested: { y: 20, z: 30 },
  b: 2,
  arr: [{ q: 20, s: 40 }, { r: 30 }, { t: 5 }]
};

// Deep merge: recurses into nested objects and merges arrays element-wise
r.expr(base).mergeDeep(over, { deep: true })
// → {
//     a: 1, b: 2,
//     nested: { x: 1, y: 20, z: 30 },
//     arr: [{ p: 1, q: 20, s: 40 }, { r: 30 }, { t: 5 }]
//   }

// Shallow mode (default, deep: false): arrays replaced wholesale
r.expr(base).mergeDeep(over)
// → arr: [{ q: 20, s: 40 }, { r: 30 }, { t: 5 }]
```

Merge semantics as implemented:

- Objects merge field-by-field; when both sides have an object at the same
  field, the merge recurses.
- Arrays merge element-wise by position when both sides have arrays; the
  right side's longer tail extends the result.
- Non-object / non-array values from the right side win.
- `mergeDeep` requires objects on both sides (`"merge_deep can only be used
  on objects"` otherwise); the function form (a lambda over the row) is
  supported like stock `merge`.

## Evidence

- `test/merge_e2e_test.py` — UPSERT semantics (update-existing vs
  insert-new, field preservation, explicit `conflict="error"`) and
  MERGE_DEEP semantics (nested recursion, element-wise array merge,
  shallow-mode array replacement), exercised through the Python driver
  against a live server.
- Terms: `src/rdb_protocol/terms/writes.cc` (`upsert`, insert with
  `conflict_behavior_t::UPDATE`), `src/rdb_protocol/terms/obj_or_seq.cc`
  (`merge_deep`).
