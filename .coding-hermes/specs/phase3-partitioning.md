# Declarative Table Partitioning

Status: axiom-level implementation specification for Phase 3.

Scope: add declarative, range-based table partitioning to the RethinkDB tree rooted
at `/home/kara/rethinkdb`. This specification defines a single supported strategy:
range partitioning over one required top-level document field. Hash, list, expression,
and nested-field partition keys are not part of this feature. The implementation must
not accept their configuration and silently interpret it as range partitioning.

The public table remains one ReQL table and one authorization object. Each partition is
an internal physical storage unit with the table's configured shard and replica policy.
Partition boundaries are configuration metadata, not user-visible tables.

## 1. Overview — declarative physical locality without changing table semantics

Declarative partitioning lets an operator state how documents in one logical RethinkDB
table are divided into bounded, independently managed ranges of a field. The partition
key is extracted from a document at write time. The server routes the mutation to the
unique active range that contains the key; callers continue to use `r.table("events")`
and receive a normal table sequence.

The primary use cases are:

- Time-series tables, where `created_at` ranges separate old, warm, and current event
  data; operators can split the current range without rewriting the table API.
- Multi-tenant data, where ordered tenant identifiers or tenant buckets provide
  administrative locality, targeted retention, and controlled movement of one tenant
  range without exposing per-tenant physical tables.
- Geo-oriented deployments whose application writes a stable, ordered region code
  (for example an S2-prefix string or administrative-region code) and needs data
  locality by region without using geospatial indexes as an ownership mechanism.

Partitioning is not RethinkDB sharding. Sharding divides the primary-key B-tree of one
physical table into replica-placement units and is configured by `shards` and
`replicas`. Partitioning first chooses a physical child store from a user document
field; each child store is then sharded using the parent table's existing shard scheme
and replica policy. A table with three partitions and two shards has six physical
partition-shard stores. Shard reconfiguration does not change partition boundaries;
partition split and merge do not change shard count or replica placement.

The feature preserves normal document identity: primary keys remain unique across the
logical table. Every write validates the global primary key before committing. A move
caused by a primary-key update is forbidden, because routing is by partition key and
primary-key uniqueness cannot be atomically checked across two partition stores in this
release. An update may change the partition key only when its new value maps to the
same partition; otherwise it fails with `PARTITION_KEY_MOVE_FORBIDDEN`.

Version 3.0 non-goals are explicit: no automatic time interval creation, no automatic
partition expiry/drop, no default partition, no multi-column key, no expression key,
no hash/list strategy, no nested paths, no cross-partition `eqJoin`/`join`, and no
cross-partition transaction. A range map must cover the complete ReQL datum domain
from `r.minval` through `r.maxval`, so a missing partition is never a valid fallback.

## 2. Interfaces — ReQL, ql2, and administration contracts

### 2.1 Table creation and configuration

The exact creation form is:

```javascript
r.tableCreate('events', {
  primary_key: 'id',
  partition: {
    type: 'range',
    key: 'created_at',
    ranges: [
      {name: 'before_2026', from: r.minval, to: r.time(2026, 1, 1, 'Z')},
      {name: '2026', from: r.time(2026, 1, 1, 'Z'), to: r.maxval}
    ]
  }
})
```

The canonical ReQL signature is:

```text
r.tableCreate(name: STRING,
              {primary_key?: STRING, shards?: NUMBER, replicas?: NUMBER|OBJECT,
               nonvoting_replica_tags?: ARRAY, primary_replica_tag?: STRING,
               durability?: "hard"|"soft",
               partition?: {type: "range", key: STRING, ranges: ARRAY<range>}})
    -> {tables_created: NUMBER, config_changes: ARRAY}

range := {name: STRING, from: DATUM, to: DATUM}
```

`from` is inclusive and `to` is exclusive. The first range must have `from ==
r.minval`; the final range must have `to == r.maxval`; each later `from` must compare
equal to the preceding `to`; names must be unique ASCII table-config identifiers; and
every range must satisfy `from < to` under `ql::datum_t::cmp`. `r.minval` and
`r.maxval` are valid only at the outer boundaries. `key` is a non-empty top-level field
name, may not equal the primary key field, and may not begin with `_`.

There are no new public `r.tableConfig()` or `r.tableReconfigure()` ql2 terms. In
existing drivers, these administrative operations are the public ReQL terms
`r.table('events').config()` and `r.table('events').reconfigure(...)`; their server
methods are named `reql_cluster_interface_t::table_config` and
`reql_cluster_interface_t::table_reconfigure`. Bindings that expose camel-case helper
methods `tableConfig` or `tableReconfigure` must compile them to these existing ql2
terms rather than introduce aliases with divergent behavior.

```text
r.table(name: STRING).config() -> SingleSelection
r.table(name: STRING).reconfigure(
    {shards: NUMBER, replicas: NUMBER|OBJECT, primary_replica_tag?: STRING,
     nonvoting_replica_tags?: ARRAY, dry_run?: BOOL,
     partition?: partition-config}) -> OBJECT
```

`config()` returns a selection from `rethinkdb.table_config` with a new `partition`
object. For an unpartitioned table it is `{type: "none", key: null, ranges: []}`. For
a partitioned table each range includes `name`, `from`, `to`, `state`, `id`, and
`config_epoch`; internal `storage_id` is not returned. Reconfigure continues to
require `shards` and `replicas` when it changes replication. A `partition` optarg is
valid only when both are absent; a call which mixes partition and placement changes is
rejected so one Raft entry has one topology purpose.

`ql2.proto` changes the comments/signatures for existing `TABLE_CREATE = 60` and
`RECONFIGURE = 176` to include the `partition` optarg. It adds the following ql2 term
IDs in the next unused administration range, reserved together in one protocol bump:

```text
PARTITION_LIST    // Table -> ARRAY
PARTITION_STATUS  // Table, STRING... -> ARRAY
PARTITION_SPLIT   // Table, STRING, DATUM, {wait?: BOOL} -> OBJECT
PARTITION_MERGE   // Table, STRING, STRING, {wait?: BOOL} -> OBJECT
PARTITION_DROP    // Table, STRING, {force?: BOOL, wait?: BOOL} -> OBJECT
PARTITION_WAIT    // Table, STRING... -> ARRAY
```

### 2.2 Partition management terms

The exact ReQL surface is:

```text
r.table(name).partitionList() -> ARRAY<partition-config>
r.table(name).partitionStatus(names?: STRING...) -> ARRAY<partition-status>
r.table(name).partitionSplit(name: STRING, at: DATUM, {wait?: BOOL})
    -> {config_changes: ARRAY, partitions_created: 2, partitions_dropped: 0}
r.table(name).partitionMerge(left: STRING, right: STRING, {wait?: BOOL})
    -> {config_changes: ARRAY, partitions_merged: 1}
r.table(name).partitionDrop(name: STRING, {force?: BOOL, wait?: BOOL})
    -> {partitions_dropped: 1, rows_deleted: NUMBER}
r.table(name).partitionWait(names?: STRING...) -> ARRAY<partition-status>
```

`partitionList()` returns configuration ordered by `from`; it is identical to the
`partition.ranges` value from `config()` except it includes no transient progress.
`partitionStatus()` returns objects containing `name`, `id`, `state`, `rows_estimate`,
`backfill_progress`, `config_epoch`, `primary_replicas`, and `error` when failed.
`partitionWait()` blocks until all requested partitions are `active`, or raises the
recorded transition failure; an empty name list means every configured partition.

`partitionSplit(name, at)` replaces active range `[from, to)` with `[from, at)` and
`[at, to)`. It accepts `from < at < to` only. The new names are deterministically
`name + "_0"` and `name + "_1"`; if either exists, the command fails instead of
inventing a suffix. `partitionMerge(left, right)` accepts only two adjacent active
ranges where `left.to == right.from`; the merged range uses `left.name` and deletes
`right.name` after drain. `partitionDrop` accepts only an active boundary range and
requires `force: true`; it removes data and extends its one adjacent neighbor to
preserve complete coverage. It rejects a table's sole partition and an internal range
that has two neighbors. `wait` defaults to `false`; commands return after durable Raft
configuration commit when false, and after `partitionWait` when true.

### 2.3 C++ call boundaries

Extend `table_generate_config_params_t` with `partition_config_t partition;`; its
default constructor sets `partition.strategy = partition_strategy_t::NONE`. Extend the
existing cluster interface exactly as follows:

```cpp
virtual bool table_partition_split(
    auth::user_context_t const &user_context,
    counted_t<const ql::db_t> db,
    const name_string_t &table,
    const name_string_t &partition_name,
    const ql::datum_t &split_at,
    bool wait,
    signal_t *interruptor,
    ql::datum_t *result_out,
    admin_err_t *error_out) = 0;

virtual bool table_partition_merge(
    auth::user_context_t const &user_context,
    counted_t<const ql::db_t> db,
    const name_string_t &table,
    const name_string_t &left_name,
    const name_string_t &right_name,
    bool wait,
    signal_t *interruptor,
    ql::datum_t *result_out,
    admin_err_t *error_out) = 0;

virtual bool table_partition_drop(
    auth::user_context_t const &user_context,
    counted_t<const ql::db_t> db,
    const name_string_t &table,
    const name_string_t &partition_name,
    bool force,
    bool wait,
    signal_t *interruptor,
    ql::datum_t *result_out,
    admin_err_t *error_out) = 0;
```

The parser implementation belongs in `src/rdb_protocol/terms/db_table.cc`; the real,
artificial, and unit-test implementations of `reql_cluster_interface_t` must all
implement these pure virtual calls. `table_config()` and `table_reconfigure()` carry
the parsed partition config through existing table metadata update paths.

## 3. Data Structures — durable configuration, serialization, and physical layout

### 3.1 Exact durable types

Add these declarations to `src/clustering/administration/tables/table_metadata.hpp`.
All fields are public intentionally, matching `table_config_t` and `sindex_config_t`.
`uint64_t` is used for durable epochs/counters rather than `size_t`.

```cpp
enum class partition_strategy_t : int8_t {
    NONE = 0,
    RANGE = 1
};

enum class partition_state_t : int8_t {
    CREATING = 0,
    ACTIVE = 1,
    DRAINING = 2,
    SPLITTING = 3,
    MERGING = 4,
    DROPPED = 5
};

ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    partition_strategy_t, int8_t,
    partition_strategy_t::NONE, partition_strategy_t::RANGE);
ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    partition_state_t, int8_t,
    partition_state_t::CREATING, partition_state_t::DROPPED);

class partition_range_t {
public:
    name_string_t name;
    uuid_u id;
    ql::datum_t from;
    ql::datum_t to;
    namespace_id_t storage_id;
    partition_state_t state;
    uint64_t config_epoch;
    uint64_t source_epoch;
    optional<uuid_u> source_partition_id;
};

class partition_config_t {
public:
    partition_strategy_t strategy;
    std::string key;
    uint64_t config_epoch;
    std::vector<partition_range_t> ranges;

    bool is_partitioned() const {
        return strategy == partition_strategy_t::RANGE;
    }
    const partition_range_t *find_active_range(const ql::datum_t &key_value) const;
    void validate_or_throw() const;
};

RDB_DECLARE_SERIALIZABLE(partition_range_t);
RDB_DECLARE_EQUALITY_COMPARABLE(partition_range_t);
RDB_DECLARE_SERIALIZABLE(partition_config_t);
RDB_DECLARE_EQUALITY_COMPARABLE(partition_config_t);
```

Extend `table_config_t` after `durability` with the exact field:

```cpp
partition_config_t partition;
```

`table_basic_config_t` gains `std::string partition_key;` and `uint64_t
partition_config_epoch;`. They are copied from `table_config_t::partition` whenever
that config is applied. This keeps every parser/table-query process aware that a table
is partitioned without duplicating the ranges in a separate incompatible basic config.
The authoritative ranges remain `table_config_t::partition`.

The constructors/default factory initialize an unpartitioned table as:

```cpp
partition_config_t{partition_strategy_t::NONE, "", 0, {}}
```

### 3.2 Serialization and Raft change records

Append the `partition` field after `durability` in the current `table_config_t`
serializer in `table_metadata.cc`; do not change the order of any existing field. Add
an explicit cluster-version gate `PARTITIONING_METADATA_VERSION`. Deserialization from
older metadata sets the default none config without attempting to read a tail. Newer
readers reject a partitioning metadata tail from an unsupported version; they must not
pretend the table is unpartitioned.

The required implementation macros are:

```cpp
RDB_IMPL_SERIALIZABLE_9_SINCE_PARTITIONING_METADATA_VERSION(
    partition_range_t, name, id, from, to, storage_id, state,
    config_epoch, source_epoch, source_partition_id);
RDB_IMPL_EQUALITY_COMPARABLE_9(
    partition_range_t, name, id, from, to, storage_id, state,
    config_epoch, source_epoch, source_partition_id);
RDB_IMPL_SERIALIZABLE_4_SINCE_PARTITIONING_METADATA_VERSION(
    partition_config_t, strategy, key, config_epoch, ranges);
RDB_IMPL_EQUALITY_COMPARABLE_4(partition_config_t, strategy, key, config_epoch, ranges);
RDB_IMPL_EQUALITY_COMPARABLE_7(
    table_config_t, basic, shards, write_hook, sindexes, write_ack_config,
    durability, partition);
```

If the generated macro arity is unavailable, implement `serialize` and `deserialize`
templates manually with precisely the field order above, then instantiate with
`INSTANTIATE_SERIALIZE_FOR_CLUSTER_AND_DISK`. The implementation must not use a
non-versioned `RDB_IMPL_SERIALIZABLE` shortcut for a table-metadata wire change.

Add these records to `table_config_and_shards_change_t` and its `boost::variant`:

```cpp
class partition_split_t {
public:
    uuid_u source_partition_id;
    partition_range_t left;
    partition_range_t right;
};
class partition_merge_t {
public:
    uuid_u left_partition_id;
    uuid_u right_partition_id;
    partition_range_t merged;
};
class partition_drop_t {
public:
    uuid_u partition_id;
    uuid_u expanded_neighbor_id;
    ql::datum_t new_neighbor_from;
    ql::datum_t new_neighbor_to;
};
```

Each record has `RDB_DECLARE_SERIALIZABLE` and a `_FOR_CLUSTER` implementation. Its
`apply_change` first validates the complete candidate `partition_config_t`; only then
increments `config_epoch` and replaces ranges. Raft state therefore never exposes a
partially edited boundary map.

### 3.3 Storage layout and routing records

`storage_id` is an internal namespace UUID. It is never included in user ReQL output
and is allocated before the Raft change is proposed. The on-disk logical layout is:

```text
tables/<logical-table-uuid>/
  partitions/<partition-uuid>/
    shards/<shard-ordinal>/
      primary/        # store_t primary B-tree and superblock
      sindexes/       # one copy of each logical secondary index
      changefeeds/    # local changefeed stamp state
```

`table_manager_t` owns a `store_t` for every `(logical table UUID, partition UUID,
shard ordinal)`. Query routing carries `partition_route_t { uuid_u partition_id;
namespace_id_t storage_id; key_range_t shard_range; uint64_t config_epoch; }`; it does
not overload `region_t` to mean a partition-key range because `region_t` is primary-key
routing metadata. This separation is mandatory when the partition key differs from the
primary key.

The checkout has `store_t` in `src/rdb_protocol/store.hpp` and uses B-tree primitives
under `src/btree/`; it does not presently declare a `btree_store_t` symbol. References
to `btree_store_t` in planning material mean this existing `store_t`/B-tree storage
layer, not a new parallel storage abstraction. Do not add a second store class merely
to satisfy that historical name.

## 4. Behavior — routing, query fan-out, indexes, and changefeeds

### 4.1 Write routing

For insert, replace, update, delete, and write-hook output, `real_table_t` resolves a
snapshot of `partition_config_t` before constructing its existing write request.
Document writes use this algorithm:

1. If `strategy == NONE`, use the existing table/shard route unchanged.
2. For insert/replace, require that `doc.has(partition.key)` and the value is neither
   `null`, an array, object, geometry, nor an unorderable ReQL pseudo-type. Convert it
   to `ql::datum_t` without stringification.
3. Binary-search sorted `ranges` by `from`, then require `from <= value < to`.
4. Route to that range's `storage_id`, then use the existing primary-key shard routing
   inside that physical partition.
5. Attach the read `config_epoch` to the write. A server receiving a stale epoch
   refreshes metadata and retries routing once; a second mismatch returns
   `PARTITION_CONFIG_CHANGED`.
6. For update/replace, read the old document's partition key. If the new value is
   absent, invalid, or maps to another `partition_range_t::id`, reject before a write
   transaction is started. Delete routes by the stored old document, not by a client
   supplied key.

Global primary-key uniqueness is maintained by a small logical-table primary-key
registry in the existing table metadata path. The registry maps primary key encoding to
partition UUID, is written in the same Raft-ordered logical mutation path, and is
consulted before an insert. It is not a user sindex and is not queryable. A conflicting
insert returns the ordinary duplicate-primary-key error. This registry is also why the
feature does not allow partition-key movement in 3.0.

### 4.2 Reads and partition pruning

All reads use one immutable `partition_config_t` snapshot. A scan with no analyzable
partition-key predicate fans out to every active partition. Each selected partition
then uses normal primary-key/sindex/shard execution. Results are merged using existing
unordered stream semantics unless the query already requests an order that RethinkDB
can fulfill locally; global `orderBy` materializes the existing normal distributed
sort after fan-in.

The planner may prune only when it proves a predicate constrains exactly the configured
partition key. Supported analyzable forms are:

```javascript
r.table('events').filter(r.row('created_at').ge(lo).and(r.row('created_at').lt(hi)))
r.table('events').filter(r.row('created_at').eq(value))
r.table('events').between(lo, hi, {index: 'created_at'})
```

The `between` form is prunable only if index `created_at` is a regular, single-value
sindex defined as exactly `r.row('created_at')`; the same validation used for the
index's reconstructed function is required. Arbitrary lambdas, JavaScript driver
functions, transforms, and indexes with `multi`, geo, FTS, vector, or BRIN flags are
not analyzed and therefore fan out safely.

For a closed/open predicate range `[L, U)`, select partition `P=[p.from, p.to)` iff
`p.from < U && L < p.to`; equality uses the caller's requested open/closed bounds.
An equality key selects exactly one partition. Build a contiguous vector of selected
routes in range order, eliminate none, and pass it to the normal multi-shard executor.
The planner records `partitions_total`, `partitions_selected`, and
`partition_pruning_applied` in the query profile.

### 4.3 Secondary indexes

A logical `sindex_config_t` remains in `table_config_t::sindexes`. `sindex_create`
creates the same definition in every active physical partition before the logical index
becomes ready. `indexWait` and `indexStatus` aggregate all active partitions: the
logical index is ready only if every partition reports ready, and progress is the
row-estimate-weighted mean of child progress. An index created while a partition is
creating is added to its child catalog before backfill starts.

Sindex writes occur only in the destination partition's `store_t`; no global secondary
B-tree is built. A sindex range query fans out to every unpruned partition and merges
child results. This is correct even when the sindex key differs from the partition key.
The partition key itself does not automatically gain a secondary index; applications
that use `between(..., {index: key})` must create the required normal sindex.

BRIN, vector, FTS, geo, multi, and compound indexes use their existing per-store
semantics. Their metadata is copied to each child store. BRIN summaries are local to a
partition-shard primary B-tree and cannot summarize other partitions. `indexDrop` waits
for child catalog removal and must release each child sidecar as it does for a normal
single table store.

### 4.4 Changefeeds

`real_table_t::read_changes` creates a logical partitioned-feed coordinator when the
table strategy is range. The coordinator obtains one configuration snapshot, registers
a `changefeed_subscribe_t` with every active partition store, obtains the usual
per-store stamps, then begins forwarding events only after all subscriptions are live.
Initial values are read from the same snapshot; state events retain existing ReQL
shapes and do not expose internal partition IDs.

A split or merge first pins the old partition configuration for existing feeds. New
subscriptions receive a metadata epoch transition: they subscribe to newly active
partitions before unsubscribing old ones, compare `(primary_key, changefeed_stamp)` at
the coordinator, and emit each committed document mutation exactly once. A child
partition's backfill is marked internal and never appears as user `new_val` events.
A feed that cannot establish the new set before its stamp retention expires terminates
with the existing changefeed overflow error rather than silently losing events.

## 5. States — lifecycle, transitions, and invariants

A partition has exactly one `partition_state_t`. `DROPPED` is retained only in the
Raft/log transition record and never remains in an active `partition_config_t::ranges`
vector after cleanup.

| State | Reads | New writes | Meaning |
| --- | --- | --- | --- |
| `CREATING` | no user routing | no | physical stores allocated; snapshot/backfill in progress |
| `ACTIVE` | yes | yes | sole authoritative route for its configured range |
| `DRAINING` | old-epoch reads only | no | superseded store retained for pinned readers/feeds |
| `SPLITTING` | source according to old epoch | source until cutover | source has two creating children |
| `MERGING` | sources according to old epoch | sources until cutover | adjacent sources are building one child |
| `DROPPED` | no | no | references released and physical stores queued for deletion |

Creation is `CREATING -> ACTIVE`; the table itself becomes write-ready only after every
initial configured partition is active. Split transitions are `ACTIVE -> SPLITTING`,
create two `CREATING` children, copy a source snapshot plus mutation log to a high-water
stamp, atomically publish a new config where children are `ACTIVE` and source is
`DRAINING`, then release the source after all old-epoch leases drain. Merge performs
the symmetric operation for two adjacent active sources and one creating destination.

A failed creation, split, or merge never publishes the target configuration. It removes
unpublished child stores and restores source state to `ACTIVE`. Once a cutover config is
Raft committed, it is immutable; retry/recovery completes cleanup but never rolls the
boundary map backward. Partition drop publishes the expanded adjacent range, drains the
dropped partition for old readers, then deletes physical data only after lease release.

Required invariants, checked by `partition_config_t::validate_or_throw()` and before
any Raft config proposal, are:

1. `NONE` has empty key, epoch zero, and no ranges.
2. `RANGE` has a non-empty key, a positive epoch, and at least one range.
3. Active ranges sort strictly by `from`, begin at `r.minval`, end at `r.maxval`, are
   contiguous, nonempty, and have unique names, IDs, and storage IDs.
4. At one config epoch, exactly one `ACTIVE` range accepts every valid key.
5. A `DRAINING`, `SPLITTING`, or `MERGING` range is not selected by a new-epoch write.
6. A physical partition's shard count, voting replicas, write acknowledgement mode,
   durability, sindex definitions, and write hook equal the parent table config.
7. A partition transition obtains a source snapshot stamp before it copies data and
   proves target replay through that stamp before the Raft cutover proposal.

## 6. Error Paths — validation, execution, backfill, and consensus failures

All public errors use `ql::base_exc_t::LOGIC` for invalid ReQL shape/value,
`OP_FAILED` for operational failures, and the existing `PERMISSION_ERROR` path for
authorization. Server-side code records a stable `partition_error_code_t` string in
status and `admin_err_t` diagnostics.

| Code / condition | Required behavior |
| --- | --- |
| `PARTITION_CONFIG_INVALID` | Reject unknown `type`, missing key/ranges, non-array ranges, non-string name/key, empty names, unsupported datum bounds, or a range map that violates any invariant. |
| `PARTITION_KEY_MISSING` | Insert/replace document does not contain the configured field; no mutation occurs. |
| `PARTITION_KEY_INVALID` | Key is null, array, object, geometry, binary-incompatible pseudo-type, or cannot be compared to configured range boundaries. |
| `PARTITION_NOT_FOUND` | A management command names no current active partition; do not treat it as a no-op. |
| `PARTITION_KEY_OUT_OF_RANGE` | Defensive error if binary search finds no active range; it indicates corrupt config and triggers config refresh/health alert. |
| `PARTITION_KEY_MOVE_FORBIDDEN` | Update/replace routes to a different partition ID; report old/new names and leave original document unchanged. |
| `PARTITION_SPLIT_INVALID` | Split point equals/exceeds source bound, source is not active, source name conflict exists, or another transition holds the source. |
| `PARTITION_MERGE_INVALID` | Names are identical, ranges are not adjacent active ranges, or either range is transitioning. |
| `PARTITION_DROP_REJECTED` | `force` absent/false, target is sole range, target is an internal range, or target is already draining. |
| `PARTITION_CONFIG_CHANGED` | Write/read route epoch changed twice during one operation; retry is delegated to the user rather than risking a stale destination. |
| `PARTITION_BACKFILL_FAILED` | Snapshot read, target write, sindex construction, or mutation-log replay failed before cutover. Leave source authoritative, remove targets, record failure in status. |
| `PARTITION_BACKFILL_OVERFLOW` | Source mutation log retained insufficient history for catch-up. Abort pre-cutover work and restart from a new source snapshot; never skip mutations. |
| `PARTITION_STORAGE_UNAVAILABLE` | Any child lacks quorum/required primary during creation or cutover. Do not publish ACTIVE; surface ordinary availability diagnostics plus partition status. |
| `PARTITION_INDEX_NOT_READY` | A query explicitly using a child sindex before the aggregate index is ready fails through existing `sindex_not_ready_exc_t`, never with a partial partition result. |
| `PARTITION_QUERY_LIMIT` | Fan-out exceeds `configured_limits_t::max_partition_fanout` (default 128) for an unpruned non-changefeed query. Raise a logic error directing the caller to constrain the partition key. Changefeeds are exempt but enforce existing feed limits. |
| `PARTITION_CONFIG_RAFT_TIMEOUT` | Raft proposal did not obtain commit confirmation before interrupt/timeout. Return `OP_FAILED`; the caller must inspect `partitionStatus` because the proposal may commit later. |
| `PARTITION_CONFIG_RAFT_CONFLICT` | Current config epoch differs from the proposal base epoch. Refresh metadata and retry only if the command is still semantically valid; otherwise return conflict. |
| `PARTITION_METADATA_INCOMPATIBLE` | A node cannot deserialize `PARTITIONING_METADATA_VERSION`. Refuse table creation/reconfiguration until every voting server supports the version. |

A partition config change is sent through the existing table metadata Raft/semilattice
path, guarded by the same config-change lock used by `table_reconfigure`. It must be
quorum committed before any router uses the new epoch. Physical child allocation may
precede that commit only as unreferenced provisional storage; it is garbage-collected
on proposal failure. A crash after commit is recovered by the table manager from durable
state and is not interpreted as permission to choose a different boundary map.

## 7. Testing — unit, integration, performance, and chaos gates

### 7.1 Unit tests

Add focused unit coverage under `src/unittest/partitioning_test.cc` using the project
pattern `namespace unittest` and `unittest/gtest.hpp`.

1. Serialization round trips for `partition_range_t`, `partition_config_t`, and a
   `table_config_t` containing range metadata, including `r.minval`/`r.maxval` bounds.
2. Backward compatibility decoding: an older serialized table config produces a
   `NONE` partition config; a truncated new tail is rejected rather than defaulted.
3. `validate_or_throw()` accepts a one-range universe and adjacent intervals, and
   rejects overlap, gap, duplicate name/id/storage ID, reversed/equal bounds, bad
   strategy/key combinations, and active range maps not covering the universe.
4. Routing property tests: generate sorted disjoint ranges and values at every boundary;
   each valid value maps to exactly one partition and no invalid value maps anywhere.
5. State-transition tests for creation, split success/failure, merge success/failure,
   drain lease release, and idempotent crash recovery. Assert that a failed pre-cutover
   transition preserves precisely the original active map.
6. Global primary-key registry tests: duplicate insert across two destination ranges
   fails, same-range duplicate remains the ordinary conflict, and key-moving updates
   do not modify either registry or document.
7. Pruning tests for equality and every open/closed range endpoint. Assert opaque
   filters select all active partitions and an exact partition-key predicate selects
   only overlapping ones.

### 7.2 Integration tests

Add ReQL workloads under `test/rql_test/` and a multi-server scenario under
`test/scenarios/`. The tests must create:

```javascript
r.tableCreate('events', {partition: {type: 'range', key: 'created_at', ranges: [
  {name: 'old', from: r.minval, to: r.time(2026, 1, 1, 'Z')},
  {name: 'new', from: r.time(2026, 1, 1, 'Z'), to: r.maxval}
]}})
```

Required black-box cases are inserts in both ranges, missing/null key rejection,
point/equality/range queries crossing one and two ranges, unconstrained full scans,
normal sindex creation before and after partition creation, `indexWait` aggregation,
sindex queries across partitions, `config()` output, invalid management arguments,
split while writes continue, merge after quiescence, forced boundary drop, restart
while a child is creating, and changefeed exactly-once checks through a split.

Run targeted tests with the established commands:

```bash
make -j$(nproc) UNIT_TESTS=1
./build/release/rethinkdb-unittest --gtest_filter='*Partition*'
test/run --verbose --jobs 2 -H all '!unit' '!cpplint' '!long' '!disabled'
```

### 7.3 Performance and chaos tests

Benchmark 100 million time-ordered documents with 12 monthly partitions; compare a
one-day range query against an unpartitioned table and require profile evidence that
11 partitions are pruned. Benchmark an unprunable scan to establish the fan-out cost,
and benchmark 1, 12, and 128 partitions under inserts. Track p50/p95 write latency,
partition selection count, child-store cache hit rate, sindex build duration, and
resident cache bytes per partition.

Chaos tests must split an active hot range while concurrent writers use hard durability,
kill/restart the current primary during source snapshot, kill a target primary during
replay, delay Raft commit acknowledgement, and force changefeed stamp pressure. After
each run compare all logical table rows and sindex query results to a serial oracle;
there must be no duplicate primary keys, missing writes, stale routing success, or
extra changefeed events.

## 8. Security — authorization, metadata integrity, and isolation

Partitioning introduces no new principal, credential, network listener, or permission
bit. `tableCreate`, `config`, `reconfigure`, and every `partition*` administration
term use the existing table/database config authorization checks in
`real_reql_cluster_interface_t`. Data reads and writes continue to authorize against
the logical parent table UUID before partition routing. A caller cannot name a physical
`storage_id` in ReQL, inspect an internal child table, or bypass parent table grants.

Configuration changes follow the existing authenticated Raft metadata path; child
allocation is initiated only after authorization succeeds and records the authenticated
actor in existing administrative audit/logging hooks. Never trust client-supplied
partition UUIDs, storage IDs, states, or epochs: ReQL may supply only range names and
bounds; IDs and states are server generated.

Partition isolation is physical and routing-based, not a new multitenancy access
control boundary. A range predicate can prune other partitions, but a user with table
read permission can still perform an unpruned table scan and read all rows. Applications
requiring tenant authorization must retain row-level/security design outside this
feature. TLS, at-rest storage, backup, and replica encryption behavior remains that of
the parent table and applies identically to every child store.

Validate all range names and key strings with `name_string_t`/existing table-config
validation before using them in paths or log messages. Paths are constructed solely
from server-generated UUIDs, never from names. Bound datums are serialized using
existing datum codecs with configured size/depth limits, preventing oversized config
objects from becoming Raft amplification payloads.

## 9. Performance — pruning, locality, resource limits, and declared limits

Partition pruning uses the sorted range vector in `partition_config_t`. A single
binary search identifies the equality destination in O(log P), where P is active
partitions. A range predicate lower-bounds its first possible overlap and walks only
until `from` reaches the upper bound: O(log P + S), where S is selected partitions.
The planner does not inspect documents or invoke sindex functions while pruning.
Opaque predicates must fan out because pruning an unknown predicate could cause false
negatives.

The coordinator sends one existing query request per selected `(partition, shard)`;
responses merge through the normal datum-stream path. Write fan-out is exactly one
partition and one existing shard route for normal mutations. Split/merge backfill is
the exception: it reads one source and writes one or two targets, and is rate-limited
by `partition_backfill_max_bytes_per_second` (default 64 MiB/s per logical table) and
`partition_backfill_max_concurrent` (default 1 per logical table). These are server
configuration values, not ReQL optargs.

Per-partition `store_t` and B-tree cache locality improve hot-range workloads: current
time ranges keep their own primary/sindex pages hot without churn from cold partitions.
The cache balancer must publish per-partition perfmon collections under
`tables.<table_uuid>.partitions.<partition_uuid>` and enforce the existing global cache
budget; partitioning does not reserve memory per range. An excessive number of tiny
partitions increases superblock, sindex, changefeed, and query-directory overhead,
which is why table creation rejects more than 128 ranges and split rejects a result
that would exceed 128 active ranges.

No cross-partition `JOIN`, `EQ_JOIN`, or `INNER_JOIN` is supported in v3.0. `eqJoin` on
a partitioned table raises `PARTITION_CROSS_JOIN_UNSUPPORTED` unless the input is
statically proved to a single partition by an equality predicate; this release does not
implement that proof, so it always raises for partitioned tables. This avoids silently
materializing unbounded cross-product/index fan-out under a familiar ReQL operation.

Performance acceptance targets are behavioral rather than hardware-specific: a
partition-key equality must select one partition; a narrow monthly predicate on the
12-month benchmark must select one; an unconstrained scan must return the same rows as
an unpartitioned reference; and all selected partition results must use bounded,
backpressured fan-in. The profile counters are the evidence used to diagnose whether
pruning helped.

## 10. Dependencies — required repository seams and implementation order

The implementation touches these existing subsystems and must preserve their ownership
boundaries:

| Subsystem | Current seam | Required partitioning change |
| --- | --- | --- |
| ReQL and ql2 | `src/rdb_protocol/terms/db_table.cc`, `src/rdb_protocol/ql2.proto` | Parse/validate `partition` on table create/reconfigure; implement management terms and protocol declarations. |
| Cluster interface | `src/rdb_protocol/context.hpp`, `src/clustering/administration/real_reql_cluster_interface.*` | Extend `table_generate_config_params_t` and `reql_cluster_interface_t`; authorize, propose, and report transitions. |
| Table metadata / Raft | `src/clustering/administration/tables/table_metadata.hpp/.cc`, Raft configuration management | Add durable config, versioned serialization, equality, transition records, atomic `apply_change`, and epoch conflict handling. |
| Table manager / rebalancer | `src/clustering/table_manager/`, contract coordinator, cluster rebalancer | Manage child stores by `(table, partition, shard)`, allocate/reclaim physical children, backfill/replay, and make rebalancer place each child according to parent shard replicas. |
| Query table | `src/rdb_protocol/real_table_t` in `real_table.cc/.hpp` | Resolve snapshot, route writes, prune reads, construct multi-partition fan-out, reject joins, and coordinate changefeeds. |
| Storage | `src/rdb_protocol/store.hpp`, `src/rdb_protocol/btree_store.cc`, `src/btree/` | Create a `store_t` per partition-shard, maintain child superblocks and primary-key registry integration, and preserve existing transactions/durability. The requested `btree_store_t` dependency maps to this existing store/B-tree layer because no current class has that exact name. |
| Secondary indexes | `src/rdb_protocol/btree.hpp`, `src/rdb_protocol/btree_store.cc`, `sindex_manager_t` | Replicate logical `sindex_config_t` into child stores; aggregate ready/status and query fan-out; preserve BRIN/vector/FTS sidecar lifecycle locally. |
| Changefeeds | `src/rdb_protocol/changefeed.*`, `changefeed_subscribe_t` in `protocol.hpp` | Subscribe/stamp per child, coordinate epoch transitions, suppress internal backfill, and guarantee exactly-once logical forwarding. |
| Artificial/mock environments | `artificial_reql_cluster_interface_t`, `src/unittest/rdb_env.*`, `src/unittest/mock_store.cc` | Implement new virtual methods and explicit partition read/write test handling so mocks fail cleanly rather than `guarantee` crashing. |

Implementation order is fixed: (1) ql2/config parsing and versioned metadata codec;
(2) table-manager child-store lifecycle and allocation; (3) write routing plus global
primary-key registry; (4) scan fan-out/pruning and sindex aggregation; (5) split/merge
backfill and Raft state machine; (6) changefeed handoff, rebalancer integration, tests,
benchmarks, and chaos gates. No management command may be exposed before its metadata,
physical store, recovery, and failure behavior are implemented end to end.

Before code is considered complete, run the Section 7 target commands plus the full
project checks specified by repository guidance (`make unit`, relevant `test/run`
workloads, and `make test` where the environment permits). Do not commit from the
feature worker; the foreman owns final review and commit.
