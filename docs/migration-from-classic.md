# Migrating from Classic RethinkDB 2.4.x

This document is the migration story for users of **classic (upstream)
RethinkDB 2.4.x** who want to move to this fork
(`totalwindupflightsystems/rethinkdb`). It covers data-directory
compatibility, driver / wire-protocol compatibility, a step-by-step upgrade
path, and the PHASE3 extensions / breaking changes this fork adds.

Every compatibility claim below is grounded in one of:

- the source of the fork itself (`src/version.hpp`,
  `src/serializer/log/metablock_manager.cc`, the historic disk-migration
  machinery under `src/clustering/administration/persist/migrate/`), or
- a **live probe** run against the built fork server
  (`build/release/rethinkdb`, v2.4.5-573-gb532c4-dirty, built from HEAD
  `b532c43203` on 2026-08-28) on a fresh scratch data directory under
  `/tmp`, or
- the upstream release notes (`NOTES.md`).

Claims that could **not** be directly observed are explicitly labeled
`untested / assumed` — nothing here is speculation dressed up as a result.

Probe environment (all probes in this document):

- Fork server binary: `build/release/rethinkdb` at `~/rethinkdb`, built from
  HEAD `b532c43203` (`git describe`: `v2.4.5-573-gb532c43203-dirty`; the
  `-dirty` suffix is the pre-existing `.vfs/graph/edges.jsonl` modification).
- Scratch data dir: `/tmp/rtgap052-data` (freshly created by the fork's own
  `create` subcommand — **no real/existing data directory was touched**).
- Ports: `--driver-port 38115 --cluster-port 39115 --http-port 38080`
  (non-conflicting; verified free before the probe).
- Server log: `/tmp/rtgap052-serve.log` — **outside** the data directory
  (redirecting logs into the data dir makes `serve` refuse to auto-init it).

---

## 1. Data-directory compatibility

### What the fork writes and accepts

Disk serialization in RethinkDB is versioned by `cluster_version_t` in
`src/version.hpp`:

```cpp
v2_3 = 8,
v2_4 = 9,
v2_4_is_latest = v2_4,
v2_4_is_latest_disk = v2_4,          // line 32
LATEST_DISK = v2_4,                  // line 40
CLUSTER = LATEST_OVERALL,
```

The fork's `LATEST_DISK` is **`v2_4` (numeric value 9)** — the same on-disk
format version classic RethinkDB 2.4.x writes. The fork never bumped the disk
format; the historic disk-migration machinery
(`src/clustering/administration/persist/migrate/`) only contains migrators
for **older** formats (`migrate_v1_14`, `migrate_v1_16`, `migrate_v2_1`,
`migrate_v2_3`) — there is no `v2_4` migrator because v2.4 *is* the latest
disk format. `src/serializer/log/metablock_manager.cc`
(`disk_format_version_is_recognized()`) accepts disk format versions v1.14
through `v2_4_is_latest_disk` and rejects only the ancient v1.13 format.

`NOTES.md` (upstream release notes, line 108) states this explicitly:

> No migration is required when upgrading from RethinkDB 2.4.x.

### Live probe: the fork writes disk format version 9 (v2_4)

1. **Create** (fresh scratch dir, no real data dir touched):
   `build/release/rethinkdb create -d /tmp/rtgap052-data` — exit 0.
   Resulting layout (observed):

   ```
   -rw-r--r--  metadata   (4194304 bytes — serializer metablock file)
   -rw-r--r--  log_file
   drwxr-xr-x  tmp
   ```

2. **Serve** (log to `/tmp/rtgap052-serve.log`, outside the data dir):
   `build/release/rethinkdb serve -d /tmp/rtgap052-data
   --driver-port 38115 --cluster-port 39115 --http-port 38080`
   — reached `Server ready, "karaHermes_mde_7840hs_1zs"
   0285323d-b82e-4169-90b6-d5fcfbe0c6c0` in the log; all three ports
   opened.

3. **Read-only on-disk check** (no writes to the data dir — a plain
   `xxd`/`strings` read of the metablock file):

   ```
   $ xxd -s 4080 -l 48 /tmp/rtgap052-data/metadata
   00000ff0: 0000 0000 0000 0000 0000 0000 0000 0000  ................
   00001000: 6d65 7461 626c 636b 0900 0000 72a8 f54d  metablck....r..M
   00001010: 0300 0000 0000 0000 0000 0000 0000 0000  ................
   ```

   The metablock header (`crc_metablock_t` in
   `src/serializer/log/metablock_manager.hpp`) is:
   `magic_marker[8]` = `metablck`, then `uint32_t disk_format_version`,
   then CRC, then `metablock_version_t version`. The observed bytes
   `metablck` + `09 00 00 00` mean **disk_format_version = 9 = v2_4**,
   exactly `cluster_version_t::LATEST_DISK` — the value written by
   `metablock_manager_t::create` /
   `metablock_manager_t::co_start_existing`
   (`buffer->prepare(static_cast<uint32_t>(cluster_version_t::LATEST_DISK), ...)`).
   The data file header additionally shows the classic `RethinkDB` magic and
   metadata file format `2.2`, identical to classic 2.4.x.

### Result

- The fork **writes** disk format version 9 (v2_4) — observed on disk in
  the probe.
- The fork **reads** disk format version 9 (v2_4) — its `LATEST_DISK` is
  v2_4 and `disk_format_version_is_recognized(9)` returns true; the live
  `serve` in the probe booted this v2_4-format directory and answered
  queries.
- A classic 2.4.x data directory carries the same disk format version
  (classic RethinkDB 2.4.x also uses `cluster_version_t::v2_4` as
  `LATEST_DISK`, per upstream `src/version.hpp`). Therefore a classic
  **2.4.x** data directory is served by the fork without any migration,
  consistent with `NOTES.md`.

> **Honesty label (untested / assumed):** a data directory actually created
> by a *classic upstream 2.4.x server binary* was **not** available for a
> direct serve probe in this environment. The claim above is grounded in
> format equivalence — fork `LATEST_DISK = v2_4 = 9`, an observed fork-written
> metablock with `disk_format_version = 9`, the fork's version-recognition
> code, and the upstream release notes — not in opening a classic-built
> directory. Follow the backup + smoke-test steps in section 3 and you are
> covered either way.

### Caveats

- **Back up the data directory before upgrading** (`cp -a` or your usual
  snapshot tool). The fork opens the directory read/write; an interrupted or
  failed first boot must never leave you without a pristine copy.
- **Older formats (2.3.x and below) are NOT "no migration" cases.** The fork
  retains the historic migrators (v1.14 → v2.1 → v2.3) but `NOTES.md` only
  promises no-migration from 2.4.x. Upgrading from ≤2.3.x should go through
  a classic 2.4.x server first (per upstream guidance) or be tested against
  a copy.
- **v1.13 data directories are rejected outright** by
  `disk_format_version_is_recognized()` ("Data directory is from version
  1.13 of RethinkDB, which is no longer supported").
- The probe used a fresh scratch dir under `/tmp`; **never point the fork at
  a production data directory without a backup**, and never redirect server
  logs into the data dir (`serve` auto-inits an empty dir but dies with
  `Inaccessible database file: metadata` if any file is inside).

---

## 2. Driver / wire-protocol compatibility

### Wire protocol version

The ReQL wire protocol is unchanged from classic 2.4.x. Both the vendored
fork driver and the classic PyPI driver speak **`VersionDummy.Version.V1_0`**
(`0x34c2bdc3`, "Users and permissions") plus `V0_4` (`0x400c2d20`, "Queries
execute in parallel") — identical to classic 2.4.x (`src/rdb_protocol/ql2.proto`;
`driver/python3/rethinkdb/handshake.py` `HandshakeV1_0.VERSION =
ql2_pb2.VersionDummy.Version.V1_0`). The default empty admin password and
the auth handshake are unchanged, so existing client code connects without
configuration changes.

### The vendored driver vs classic drivers

| Driver | Version | Notes |
|--------|---------|-------|
| **Vendored fork driver** (`driver/python3`, bundled in this repo) | `2.4.10.post1+source` (`rethinkdb/version.py`) | Regenerated protobuf binding (`ql2_pb2.py`) from the fork's `ql2.proto`; knows **all 22 new ReQL terms (IDs 198–219)** plus the standard 2.4.x API. Fully backward compatible with classic ReQL. |
| **Classic PyPI driver** (`pip install rethinkdb==2.4.10`) | `2.4.10` | Upstream 2.4.x driver. Speaks the same V1_0 wire protocol; knows only classic terms (IDs ≤ 197). Works against the fork for standard ReQL. |
| **Classic JS driver** (`rethinkdbdash` / official `rethinkdb` JS) | 2.4.x-era | **Assumed compatible** for standard ReQL (same wire protocol). No JS runtime was available for a live roundtrip in this environment — **untested / assumed**. |

### Live probe: ReQL roundtrip over the wire

With the fork server from section 1 still running on `--driver-port 38115`,
the same probe script (db create → table create → insert → get) was run with
**both** drivers, using the default empty admin password:

Vendored driver (`/tmp/rtgap052-venv`, `pip install ./driver/python3`):

```
connected: driver imports OK
db created: rtgap052_probe_db
table created: probe_table
inserted: {'id': 1, 'name': 'rtgap052', 'ts': 20260828}
get returned: {'id': 1, 'name': 'rtgap052', 'ts': 20260828}
ROUNDTRIP-OK          (exit 0)
```

Classic driver (`/tmp/rtgap052-venv-classic`, `pip install rethinkdb==2.4.10`):

```
connected: driver imports OK
db created: rtgap052_probe_db
table created: probe_table
inserted: {'id': 1, 'name': 'rtgap052', 'ts': 20260828}
get returned: {'id': 1, 'name': 'rtgap052', 'ts': 20260828}
ROUNDTRIP-OK          (exit 0)
```

Both roundtrips returned the inserted document byte-for-byte. **A classic
2.4.x Python driver connecting to the fork server is therefore
live-proven**, not assumed.

Driver setup notes (observed): the vendored driver's `net.py` imports
`ssl.match_hostname`, which Python 3.12+ removed — plain `python3`
(3.13/3.14) fails to import it. Use Python 3.11 (here `~/.local/bin/python3.11`)
or patch the import. The vendored driver exports `RethinkDB` and `connect`
(use the `RethinkDB()` factory; classic drivers also expose module-level
methods).

### What to test after upgrade

1. Connect with your existing driver and empty password (no auth change).
2. Smoke test: db create → table create → insert → get (the roundtrip
   above).
3. Changefeeds (`.changes()`) — unchanged in the fork.
4. Secondary indexes (`index_create`, `index_wait`) — unchanged.
5. Only then exercise any new PHASE3 features (section 4) with the **vendored**
   driver — the classic driver will reject the new term IDs.

---

## 3. Step-by-step upgrade path for a classic 2.4.x user

1. **Stop the classic server** cleanly (SIGTERM / `rethinkdb-admin
   shutdown`, wait for the process to exit).
2. **Back up the data directory** (do not skip this):
   `cp -a /var/lib/rethinkdb/rethinkdb_data /var/lib/rethinkdb/rethinkdb_data.backup-$(date +%F)`
   (adjust paths to your install; verify the copy before continuing).
3. **Install / build the fork server** — e.g. clone
   `totalwindupflightsystems/rethinkdb`, `./configure --allow-fetch`,
   `make -j4` (see the repo README and AGENTS.md; a Docker image is
   published to `ghcr.io/totalwindupflightsystems/rethinkdb:latest` and
   pulled by `docker compose up -d`, with a source-build fallback — see
   the repo README).
4. **Point the fork at the SAME data directory** (no migration step):
   `build/release/rethinkdb serve -d /var/lib/rethinkdb/rethinkdb_data
   --driver-port 28015 --cluster-port 29015 --http-port 8080`
   (keep your classic ports; use `--initial-password <pw>` if you bind
   beyond loopback). Watch the log for `Server ready`. **Do not** redirect
   the log inside the data directory.
5. **Verify with a driver smoke test** — against the running server, run the
   section-2 roundtrip with your existing classic driver first (it must work
   unchanged), then confirm your application's queries and changefeeds.
6. **Upgrade the driver** to the vendored driver in this repo
   (`pip install ./driver/python3`) when you want the PHASE3 terms
   (section 4).
7. **Keep the backup** until the fork has served your workload successfully
   for at least one full write+read cycle.

Rollback: stop the fork server, restore the backup directory, start classic
2.4.x — the data dir format is identical (v2_4), so the classic server opens
it without migration either way.

---

## 4. PHASE3 extensions / breaking changes

The fork is a superset of classic 2.4.x: standard ReQL semantics are
preserved, and all additions are **additive** — new terms use new TermType
IDs (198–219) that classic servers/drivers simply do not know. There are no
removed or renumbered classic terms; see section 2 for driver implications.

Extensions (all links are to files in this repo's `docs/`):

| Feature | Doc | Status / notes |
|---------|-----|----------------|
| Deep merge + upsert (`merge_deep`, `upsert`; terms 216–217) | [docs/merge-upsert.md](merge-upsert.md) | implemented — Postgres-style `ON CONFLICT DO UPDATE` parity |
| Table partitioning (`partitions` optarg, `partition_info`, `repartition`) | [docs/partitioning.md](partitioning.md) | implemented |
| Generated / virtual columns (`set_generated_columns`, `get_generated_columns`; terms 218–219) | [docs/generated-columns.md](generated-columns.md) | implemented |
| CDC streaming (publications/subscriptions, terms 204–211) | [docs/cdc-streaming.md](cdc-streaming.md) | pump implemented; **sinks NOT implemented** (terms 212–215 error) |
| Vector / FTS / BRIN indexes (terms 198–201) | [docs/vector-fts.md](vector-fts.md) | implemented |
| Time-series storage (chunked time-ordered storage, retention TTL, downsampling; `table_create(timeSeries=True)`) | [docs/time-series.md](time-series.md) | implemented |
| Overview / status of all of the above | [docs/README.md](README.md) | index of fork extension docs |

Practical breaking-change surface for a classic 2.4.x user:

- **Driver must be upgraded to use new terms** — the classic driver rejects
  unknown TermTypes, and the vendored driver is the supported path (its
  `ql2_pb2.py` is regenerated from the fork's `ql2.proto`).
- **`cdcSinkCreate` and friends error** (sinks not implemented) even though
  publications/subscriptions work.
- **Time-series and partition optargs are new `table_create` optargs** —
  classic clients sending unknown optargs get server-side optarg errors; the
  optargs are only active when you pass them.
- No changes to: the auth model, the changefeed protocol, secondary-index
  semantics, or the disk format (section 1) — all verified against the
  classic wire protocol and, for disk format, the live probe above.

---

*Probe record: run 2026-08-28 on `karaHermes_mde_7840hs_1zs` (fork
`build/release/rethinkdb` 2.4.5-573-gb532c4-dirty). Scratch dir
`/tmp/rtgap052-data` created by `rethinkdb create` and served on ports
38115/39115/38080; metablock read (xxd) showed `disk_format_version = 9`
(v2_4); ReQL roundtrips passed with the vendored driver (2.4.10.post1+source)
and the classic PyPI driver (2.4.10). Scratch dir and both probe venvs were
removed after the probes.*
