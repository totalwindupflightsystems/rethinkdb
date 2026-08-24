# Fork Extension Documentation

This directory documents the PHASE3 extensions this fork adds on top of
upstream RethinkDB. Each doc covers one feature area: what it does, how to
use it from a driver, and where the implementation lives. Status is
**implemented** unless a doc explicitly says otherwise (design-spec-only
features live under `.coding-hermes/specs/`, see the main README's
"Fork extensions" table).

| Doc | Status | Feature |
|-----|--------|---------|
| [cdc-streaming.md](cdc-streaming.md) | pump implemented / sinks not implemented | CDC / logical replication: publications, subscriptions; sink drivers (Kafka / Webhook / File-S3) are NOT implemented — `cdcSinkCreate` errors |
| [merge-upsert.md](merge-upsert.md) | implemented | Deep merge (`merge_deep`) and upsert terms |
| [time-series.md](time-series.md) | implemented | Chunked time-ordered storage, `between()` chunk pruning, retention TTL, downsampling |
| [partitioning.md](partitioning.md) | implemented | Declarative table partitioning (`partitions` optarg, `partition_info`, `repartition`) |
| [generated-columns.md](generated-columns.md) | implemented | Generated / virtual columns (`set_generated_columns`, `get_generated_columns`) |
| [vector-fts.md](vector-fts.md) | implemented | Vector similarity search, full-text search, BRIN block-range indexes |

Driver entry points: the vendored Python driver
(`driver/python3/rethinkdb`, see its
[README](../driver/python3/README.md) for the term table) exposes query
methods for every implemented term; the JS driver docs are in each feature
doc above.

Quickstart and port/bind caveats: see the main [README](../README.md) and
[AGENTS.md](../AGENTS.md).
