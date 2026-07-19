// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef CLUSTERING_ADMINISTRATION_TABLES_TABLE_METADATA_HPP_
#define CLUSTERING_ADMINISTRATION_TABLES_TABLE_METADATA_HPP_

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "buffer_cache/types.hpp"   // for `write_durability_t`
#include "btree/reql_specific.hpp"  // partition_store_ref_t for set_partition_config_t
#include "clustering/administration/servers/server_metadata.hpp"
#include "clustering/generic/nonoverlapping_regions.hpp"
#include "containers/name_string.hpp"
#include "containers/uuid.hpp"
#include "rdb_protocol/partition_config.hpp"
#include "rdb_protocol/protocol.hpp"
#include "rdb_protocol/publication.hpp"  // publication_config_t for publications map
#include "rdb_protocol/subscription.hpp"  // subscription_config_t for subscriptions map
#include "rpc/connectivity/server_id.hpp"
#include "rpc/semilattice/joins/macros.hpp"
#include "rpc/serialize_macros.hpp"

/* This is the metadata for a single table. */

/* `table_basic_config_t` contains the subset of the table's configuration that the
parser needs to process queries against the table. A copy of this is stored on every
thread of every server for every table. */
class table_basic_config_t {
public:
    name_string_t name;
    database_id_t database;
    std::string primary_key;
    partition_type_t partition_type = partition_type_t::NONE;
    std::string partition_key_field;
};

RDB_DECLARE_SERIALIZABLE(table_basic_config_t);
RDB_DECLARE_EQUALITY_COMPARABLE(table_basic_config_t);

enum class write_ack_config_t {
    SINGLE,
    MAJORITY
};
ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
    write_ack_config_t,
    int8_t,
    write_ack_config_t::SINGLE,
    write_ack_config_t::MAJORITY);

/* `table_config_t` describes the complete contents of the `rethinkdb.table_config`
artificial table. */

class table_config_t {
public:
    class shard_t {
    public:
        std::set<server_id_t> voting_replicas() const;

        /* `nonvoting_replicas` must be a subset of `all_replicas`. `primary_replica`
        must be in `all_replicas` and not in `nonvoting_replicas`. */
        std::set<server_id_t> all_replicas;
        std::set<server_id_t> nonvoting_replicas;
        server_id_t primary_replica;
    };
    table_basic_config_t basic;
    std::vector<shard_t> shards;
    std::map<std::string, sindex_config_t> sindexes;
    optional<write_hook_config_t> write_hook;
    write_ack_config_t write_ack_config;
    write_durability_t durability;
    partition_config_t partitioning;
};

RDB_DECLARE_EQUALITY_COMPARABLE(table_config_t);

RDB_DECLARE_SERIALIZABLE(table_config_t::shard_t);
RDB_DECLARE_EQUALITY_COMPARABLE(table_config_t::shard_t);

class table_shard_scheme_t {
public:
    std::vector<store_key_t> split_points;

    static table_shard_scheme_t one_shard() {
        return table_shard_scheme_t();
    }

    size_t num_shards() const {
        return split_points.size() + 1;
    }

    key_range_t get_shard_range(size_t i) const;
    size_t find_shard_for_key(const store_key_t &key) const;
};

RDB_DECLARE_SERIALIZABLE(table_shard_scheme_t);
RDB_DECLARE_EQUALITY_COMPARABLE(table_shard_scheme_t);

/* `table_config_and_shards_t` exists because the `table_config_t` needs to be changed in
sync with the `table_shard_scheme_t` and the server name mapping. */

class table_config_and_shards_t {
public:
    table_config_t config;
    table_shard_scheme_t shard_scheme;

    /* This contains an entry for every server mentioned in the config. The `uint64_t`s
    are server config versions. */
    server_name_map_t server_names;

    /* CDC publications attached to this table. CDC-05a: keyed by publication_id.
    Committed through Raft via table_config_and_shards_change_t::publication_create_t /
    publication_drop_t. The map is empty on tables that have no publications. */
    std::map<uuid_u, ql::publication_config_t> publications;

    /* CDC subscriptions attached to this table's publications. CDC-06a: keyed
    by subscription_id. Committed through Raft via
    table_config_and_shards_change_t::subscription_create_t / subscription_drop_t.
    The map is empty on tables that have no subscriptions. */
    std::map<uuid_u, ql::subscription_config_t> subscriptions;
};

RDB_DECLARE_SERIALIZABLE(table_config_and_shards_t);
RDB_DECLARE_EQUALITY_COMPARABLE(table_config_and_shards_t);

class table_config_and_shards_change_t {
public:
    class set_table_config_and_shards_t {
    public:
        table_config_and_shards_t new_config_and_shards;
    };

    class write_hook_create_t {
    public:
        write_hook_config_t config;
    };

    class write_hook_drop_t {
    public:
    };

    class sindex_create_t {
    public:
        std::string name;
        sindex_config_t config;
    };

    class sindex_drop_t {
    public:
        std::string name;
    };

    class sindex_rename_t {
    public:
        std::string name;
        std::string new_name;
        bool overwrite;
    };

    /* Raft cutover for online repartitioning (PART-07 / section 5.3 step 7).
    `expected_epoch` must match the live config epoch or the change is rejected
    (concurrent repartition / stale proposal). `provisional_stores` are the
    target partition-shard store refs allocated before proposal; they become
    routable only after this change commits with epoch E+1. */
    class set_partition_config_t {
    public:
        uint64_t expected_epoch;
        partition_config_t new_config;
        std::vector<partition_store_ref_t> provisional_stores;
    };

    /* CDC-05a: register a publication on this table. Replaces the stub createPublication
    backend with a real Raft-propagated metadata change. The publication is inserted into
    `table_config_and_shards_t::publications` keyed by `config.publication_id`; apply_change
    returns false if a publication with that id already exists. The filter, format, and
    snapshot-mode fields on the config drive downstream change-record capture (CDC-03) and
    replication-slot allocation (CDC-07+); CDC-05a only commits the metadata. */
    class publication_create_t {
    public:
        ql::publication_config_t config;
    };

    /* CDC-05a counterpart: drop a publication by id. `name` is echoed in audit logs and
    is required by the change's wire contract but is not consulted by apply_change — the
    map is keyed by publication_id. apply_change returns false if the publication does not
    exist (idempotent re-drop is rejected here; callers should re-read config to confirm). */
    class publication_drop_t {
    public:
        uuid_u publication_id;
        name_string_t name;
    };

    /* CDC-06a: register a subscription against a publication on this table.
    Replaces the stub createSubscription backend with a real Raft-propagated
    metadata change. `table_uuid` identifies the publication's source table
    (the table whose `table_config_and_shards_t::subscriptions` map will hold
    the entry); it must match the receiver's table_id, since subscriptions are
    scoped to the publication's source table. The subscription is inserted
    keyed by `config.subscription_id`; apply_change returns false if a
    subscription with that id already exists. CDC-06a only commits the
    metadata — replication-slot allocation and sink routing are CDC-07+. */
    class subscription_create_t {
    public:
        uuid_u table_uuid;
        ql::subscription_config_t config;
    };

    /* CDC-06a counterpart: drop a subscription by id. `name` is echoed in
    audit logs and is required by the change's wire contract but is not
    consulted by apply_change — the map is keyed by subscription_id.
    apply_change returns false if the subscription does not exist (idempotent
    re-drop is rejected here; callers should re-read config to confirm). */
    class subscription_drop_t {
    public:
        uuid_u table_uuid;
        uuid_u subscription_id;
        name_string_t name;
    };

    table_config_and_shards_change_t() { }

    explicit table_config_and_shards_change_t(set_table_config_and_shards_t &&_change)
        : change(std::move(_change)) { }
    explicit table_config_and_shards_change_t(sindex_create_t &&_change)
        : change(std::move(_change)) { }
    explicit table_config_and_shards_change_t(sindex_drop_t &&_change)
        : change(std::move(_change)) { }
    explicit table_config_and_shards_change_t(sindex_rename_t &&_change)
        : change(std::move(_change)) { }
    explicit table_config_and_shards_change_t(write_hook_create_t &&_change)
        : change(std::move(_change)) { }
    explicit table_config_and_shards_change_t(write_hook_drop_t &&_change)
        : change(std::move(_change)) { }
    explicit table_config_and_shards_change_t(set_partition_config_t &&_change)
        : change(std::move(_change)) { }
    explicit table_config_and_shards_change_t(publication_create_t &&_change)
        : change(std::move(_change)) { }
    explicit table_config_and_shards_change_t(publication_drop_t &&_change)
        : change(std::move(_change)) { }
    explicit table_config_and_shards_change_t(subscription_create_t &&_change)
        : change(std::move(_change)) { }
    explicit table_config_and_shards_change_t(subscription_drop_t &&_change)
        : change(std::move(_change)) { }


    /* Note, it's important that `apply_change` does not change
    `table_config_and_shards` if it returns false. */
    bool apply_change(table_config_and_shards_t *table_config_and_shards) const;

    bool name_and_database_equal(const table_basic_config_t &table_basic_config) const;

    RDB_MAKE_ME_SERIALIZABLE_1(table_config_and_shards_change_t, change);

private:
    boost::variant<
        set_table_config_and_shards_t,
        sindex_create_t,
        sindex_drop_t,
        sindex_rename_t,
        write_hook_create_t,
        write_hook_drop_t,
        set_partition_config_t,
        publication_create_t,
        publication_drop_t,
        subscription_create_t,
        subscription_drop_t> change;

    class apply_change_visitor_t;
};

RDB_DECLARE_SERIALIZABLE(table_config_and_shards_change_t::set_table_config_and_shards_t);
RDB_DECLARE_SERIALIZABLE(table_config_and_shards_change_t::write_hook_create_t);
RDB_DECLARE_SERIALIZABLE(table_config_and_shards_change_t::write_hook_drop_t);
RDB_DECLARE_SERIALIZABLE(table_config_and_shards_change_t::sindex_create_t);
RDB_DECLARE_SERIALIZABLE(table_config_and_shards_change_t::sindex_drop_t);
RDB_DECLARE_SERIALIZABLE(table_config_and_shards_change_t::sindex_rename_t);
RDB_DECLARE_SERIALIZABLE(table_config_and_shards_change_t::set_partition_config_t);
RDB_DECLARE_SERIALIZABLE(table_config_and_shards_change_t::publication_create_t);
RDB_DECLARE_SERIALIZABLE(table_config_and_shards_change_t::publication_drop_t);
RDB_DECLARE_SERIALIZABLE(table_config_and_shards_change_t::subscription_create_t);
RDB_DECLARE_SERIALIZABLE(table_config_and_shards_change_t::subscription_drop_t);

#endif // CLUSTERING_ADMINISTRATION_TABLES_TABLE_METADATA_HPP_
