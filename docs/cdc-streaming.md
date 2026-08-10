# RethinkDB CDC Streaming Extension

**Status: implemented.** Change-data-capture for this fork: publish table
changes as a logical stream, subscribe to a publication, and route the stream
to external sinks. Covered by integration tests (see [Evidence](#evidence)).

## Overview

CDC turns a table's write activity into a consumable, replayable stream.

| Concept | Term | Description |
|---------|------|-------------|
| Publication | `publicationCreate` (term `publication_create`) | A named logical stream over a table's changes, with optional filtering (fields, operations, predicate) |
| Subscription | `subscriptionCreate` (term `subscription_create`) | Applies a publication's changes to a target table |
| Sink | `cdcSinkCreate` (term `cdc_sink_create`) | Delivers a publication's changes to an external destination: Kafka, Webhook, file, or S3 |

Writes flow through a CDC-instrumented write path (`rdb_write_visitor_t`,
`btree_store_t`); changefeeds (`changes()`) deliver the same events with
`old_val` / `new_val`.

## Creating a publication

```javascript
r.db("app").table("events").publicationCreate({
  name: "events_pub",
  format: "json",            // "json" or "internal_rdb_v1"; default "json"
  snapshot: "initial",       // "initial" or "none"; default "initial"
  filter: {                  // optional
    fields: ["id", "payload"],            // project to top-level fields
    operations: ["insert", "update"],     // "insert" | "update" | "delete" | "replace"
    predicate: { type: "create" }         // top-level equality constraints
  }
})
// → { created: 1, publication: "events_pub" }
```

Notes:

- `filter.fields` entries must be top-level field names (nested paths and
  regexes are rejected).
- `filter.predicate` is validated (top-level keys, scalar or `{in: [...]}`
  values); full predicate storage/compilation is part of the CDC filter
  pipeline.
- Without a filter, all fields and all operations are published.

## Inspecting and dropping publications

```javascript
r.db("app").table("events").publicationList()
// → [{ name: "events_pub", ... }, ...]

r.db("app").table("events").publicationStatus("events_pub")
// → { name: "events_pub", format: "json", snapshot: "initial", ... }

r.db("app").table("events").publicationDrop("events_pub")
// → { dropped: 1 }
```

Publication states reported by status: `creating`, `ready`, `dropping`,
`dropped`, `error`.

## Creating a subscription

```javascript
r.db("app").tableCreate("events_sub")

r.db("app").table("events").subscriptionCreate({
  name: "events_sub",
  publication: "events_pub",
  target: { db: "app", table: "events_sub" },   // or targetTable: "events_sub"
  conflict: "replace",         // optional; conflict handling for target writes
  snapshot: "initial"          // optional; whether to apply the initial snapshot
})
// → { created: 1, subscription: "events_sub" }
```

A subscription replays the publication into the target table. Manage it with
`subscriptionList()`, `subscriptionStatus("events_sub")`, and
`subscriptionDrop("events_sub")`.

## Creating a sink

```javascript
// File sink (writes JSON records under a path)
r.db("app").table("events").cdcSinkCreate({
  name: "file_sink",
  publication: "events_pub",
  type: "file",
  format: "json",
  path: "/var/lib/cdc/events"        // or connection: "..."
})

// Kafka sink
r.db("app").table("events").cdcSinkCreate({
  name: "kafka_sink",
  publication: "events_pub",
  type: "kafka",
  brokers: ["kafka-1:9092", "kafka-2:9092"],   // or connection: "host:port,host:port"
  topic: "app.events"
})

// Webhook sink
r.db("app").table("events").cdcSinkCreate({
  name: "webhook_sink",
  publication: "events_pub",
  type: "webhook",
  url: "https://hooks.example.com/ingest"      // or connection: "..."
})

// S3 sink
r.db("app").table("events").cdcSinkCreate({
  name: "s3_sink",
  publication: "events_pub",
  type: "s3",
  bucket: "my-bucket",                          // or connection: "..."
  prefix: "events",                             // or topic: "..."
  credentialRef: "my-s3-credentials"            // or credential_ref / credentialsRef
})
// → { created: true, sink: "file_sink", state: "creating" }
```

Sink types are `kafka`, `webhook`, `file`, and `s3` (unknown types are
rejected). Credentials must be supplied by reference (`credentialRef`,
`credential_ref`, or `credentialsRef`) — inline `password` / `secret` /
`token` fields are rejected.

Optional batching and dead-letter configuration apply to all sink types:

```javascript
r.db("app").table("events").cdcSinkCreate({
  name: "batched_sink",
  publication: "events_pub",
  type: "file",
  path: "/var/lib/cdc/events",
  batching: {
    batchSize: 500,              // or max_records; [1, 1000000]
    maxInFlightBatches: 4,       // or max_in_flight_batches; [1, 1000]
    flushIntervalMs: 5000,       // or flush_interval_ms; [1, 3600000]
    maxBufferBytes: 67108864     // or max_buffer_bytes; [1024, 1 GiB]
  },
  deadLetter: {                  // or dead_letter
    type: "file",                // kafka | webhook | file | s3
    path: "/var/lib/cdc/dlt"
  }
})
```

Manage sinks with `cdcSinkList()`, `cdcSinkStatus("file_sink")`, and
`cdcSinkDrop("file_sink")`.

## Reading the stream

Any consumer can read the same changes the CDC path captures:

```javascript
r.db("app").table("events").changes({ includeInitial: true })
// → { old_val: null, new_val: { id: 1, ... } }
// → { old_val: {...}, new_val: {...} }   // updates carry both sides
```

## Evidence

- `test/cdc_e2e_test.py` — full ReQL path for `publication_create`,
  `publication_list`, `publication_status`, `publication_drop`,
  `subscription_create`, `subscription_list`, `subscription_status`,
  `cdc_sink_create`, `cdc_sink_list`, `cdc_sink_status`, and changefeed
  delivery after writes.
- `test/cdc_integration_test.py` — CRUD, changefeed delivery
  (`old_val`/`new_val`), index operations, server-restart durability, and
  bulk/edge cases through the CDC-instrumented write path.
- Terms: `src/rdb_protocol/terms/cdc_publication.cc`,
  `src/rdb_protocol/terms/cdc_subscription.cc`,
  `src/rdb_protocol/terms/cdc_sink.cc`.
