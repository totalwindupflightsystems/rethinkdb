# RethinkDB Table Partitioning

**Status: implemented.** This fork adds logical partitioning on top of
RethinkDB's existing sharding: a table's documents are divided into named
partitions by a partition key, using range, hash, or list rules. Covered by
integration tests (see [Evidence](#evidence)).

## Overview

Partitioning complements sharding: shards distribute data across servers,
while partitions divide the key space of a table into named, routable
segments.

| Term | Purpose |
|------|---------|
| `tableCreate` `partitions` optarg | Create a partitioned table |
| `partitionInfo` | Inspect the current partition configuration |
| `repartition` | Replace the partition configuration |

Partition types:

| Type | Config | Routing |
|------|--------|---------|
| `range` | `ranges: [{name, from, to}, ...]` | Document key falls in `[from, to)` |
| `hash` | `partitions: [{name, buckets: [...]}], modulus` | `hash(key) % modulus` in the partition's buckets |
| `list` | `partitions: [{name, values: [...]}]` | Document key equals a listed value |

## Creating a partitioned table

Pass a `partitions` object to `tableCreate`:

```javascript
// Range partitioning on a "region" field
r.db("app").tableCreate("events", {
  partitions: {
    by: "region",                 // required: partition key field (string)
    type: "range",                // required: "range", "hash", or "list"
    ranges: [                     // range type: required, non-empty, contiguous
      { name: "na", from: "NA", to: "EU" },
      { name: "eu", from: "EU", to: "APAC" },
      { name: "apac", from: "APAC", to: "ZZZ" }
    ]
  }
})

// Hash partitioning (modulus + named bucket sets)
r.db("app").tableCreate("users", {
  partitions: {
    by: "user_id",
    type: "hash",
    modulus: 16,                  // required: positive integer
    partitions: [
      { name: "p0", buckets: [0, 1, 2, 3] },
      { name: "p1", buckets: [4, 5, 6, 7] },
      { name: "p2", buckets: [8, 9, 10, 11] },
      { name: "p3", buckets: [12, 13, 14, 15] }
    ]
  }
})

// List partitioning (explicit key values, optional default partition)
r.db("app").tableCreate("tenants", {
  partitions: {
    by: "tenant",
    type: "list",
    partitions: [
      { name: "acme", values: ["acme", "acme-eu"] },
      { name: "globex", values: ["globex"] },
      { name: "other", default: true }
    ]
  }
})
```

Validation (as implemented):

- `partitions` must be an object with a string `by` (non-empty partition key
  field) and a string `type` of `range`, `hash`, or `list`.
- `range` requires a non-empty `ranges` array; each entry needs `name`,
  `from`, `to`, and ranges must be contiguous (each `from` equals the
  previous `to`).
- `hash` requires a positive-integer `modulus` and a `partitions` array; each
  entry needs `name` and `buckets` (non-negative integers).
- `list` requires a `partitions` array; each entry needs `name`, with
  optional `values` (array) and `default` (boolean).
- Partition names are subject to the same character rules as table names.
- Invalid configs fail with `PARTITION_CONFIG_INVALID` /
  `PARTITION_RANGE_INVALID` / `PARTITION_HASH_INVALID` /
  `PARTITION_LIST_INVALID`.

The `partitions` optarg composes with the standard `tableCreate` optargs
(`primaryKey`, `shards`, `replicas`, `durability`, and the time-series
`timeSeries` object) — partitioning and time-series can be used together.

## Inspecting partition configuration

```javascript
r.db("app").table("events").partitionInfo()
// → { by: "region", type: "range", ranges: [...], partitions: [...], ... }
```

## Repartitioning

`repartition(newConfig, {dryRun, wait})` replaces the partition config:

```javascript
r.db("app").table("events").repartition(
  {
    by: "region",
    type: "list",
    partitions: [
      { name: "americas", values: ["NA", "SA"] },
      { name: "rest", default: true }
    ]
  },
  { dryRun: false, wait: true }
)
```

- `dryRun` (boolean, default `false`) validates the config without applying
  it.
- `wait` (boolean, default `true`) blocks until the transition completes.

Writes whose partition key does not route to any partition surface the
partition error surface (`PARTITION_KEY_MISSING`,
`PARTITION_KEY_UNROUTABLE`, etc.).

## Evidence

- `test/ts6_cluster_e2e_probe.py` — 2-shard time-series table on a 3-node
  cluster created with `tableCreate(..., shards: 2, replicas: 2, timeSeries:
  {...})`, chunk metadata visible across nodes, cluster reads, retention, and
  failover/restart consistency.
- `test/partition_diag.py` — single-node `partitionInfo` probe.
- Terms: `src/rdb_protocol/terms/partitioning.cc` (`partition_info`,
  `repartition`), `src/rdb_protocol/terms/db_table.cc` (`partitions` optarg),
  config parsing in `src/rdb_protocol/partition_config.hpp`.
