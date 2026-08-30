# Fork Revival Roadmap (v2.5 → v3.0)

This document maps the revival targets for the
`totalwindupflightsystems/rethinkdb` fork: the **v2.5 revival line** (repo
anchor + clean version reporting) and the **v3.0 major milestone** (PHASE3
extensions as release-grade features). Status is per feature and updated as
work lands.

## v2.5 Revival Line

The v2.5 line re-anchors the fork's version identity. Upstream's last release
was v2.4.5; the fork had been reporting dirty describe output
(`2.4.5-575-g337c9f-dirty`) because no v2.5/3.0 tag existed. The repo anchor
fixes that so builds are repo-grounded.

| Feature | Status | Notes |
|---|---|---|
| Repository anchor: `VERSION.OVERRIDE` = `2.5.0` | **DONE** — RT-GAP-050 (this change) | `scripts/gen-version.sh` short-circuits on the root-relative override; prints clean `2.5.0`, cwd-independent, no `-dirty` suffix even with fleet-state files dirty in the worktree |
| Annotated tag `v2.5.0` | **DONE** — RT-GAP-050 (this change) | Points at the anchor commit; pushed to origin |
| `docs/roadmap.md` (this file) | **DONE** — RT-GAP-050 (this change) | Maps v2.5 and v3.0 targets with per-feature status |
| Release-version banner / clean version output | **UNBLOCKED** — RT-GAP-054 | Previously `2.4.5-575-g337c9f-dirty` / misidentified; now resolves cleanly to `2.5.0` via existing plumbing |
| Wire/disk serialization enums (`src/version.hpp`) | Untouched (intentional) | These are wire/disk **serialization** versions — NOT the release version; do not conflate |

## v3.0 Major Milestone

v3.0 is the fork's flagship revival: the PHASE3 extensions already on `main`
as building blocks graduate into a coherent next-major release.

| Feature | Status | Notes |
|---|---|---|
| Time-series: retention TTL, downsampling, chunked storage, `between()` pruning | Building block on `main` | PHASE3-TS-1..6 (incl. cluster integration + chaos); release-grade polish and docs pending for v3.0 |
| CDC streaming: publications, subscriptions, Kafka/Webhook/File-S3 sinks | Building block on `main` | Driver E2E coverage in-repo (`test/cdc_integration_test.py`) |
| Vector indexes (HNSW / IVFFlat), FTS, BRIN | Building block on `main` | Term IDs 198–215 era features; sidecar read-path hardened |
| Online table partitioning | Building block on `main` | `PART-07`; repartition pipeline live |
| MERGE_DEEP (217) + UPSERT (216) ReQL terms | Building block on `main` | Vendored Python driver has parity for all 18 new terms |
| Generated / virtual columns (`SET_GENERATED_COLUMNS` / `GET_GENERATED_COLUMNS`, 218/219) | Building block on `main` | Raft metadata + write-path compute |
| v3.0 release cut | **Next milestone** | Needs release-line decisions (versioning scheme, upgrade path, docs sweep) — see per-feature rows above for the constituent work |
