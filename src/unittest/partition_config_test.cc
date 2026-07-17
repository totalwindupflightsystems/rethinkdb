// Copyright 2026 RethinkDB, all rights reserved.
/* PART-10 / architecture §8.1 items 1–5:
 * serialization, validation, and routing for partition_config_t. */
#include "rdb_protocol/partition_config.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "btree/reql_specific.hpp"
#include "clustering/administration/tables/table_metadata.hpp"
#include "containers/archive/stl_types.hpp"
#include "containers/archive/vector_stream.hpp"
#include "containers/archive/versioned.hpp"
#include "containers/name_string.hpp"
#include "containers/uuid.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/error.hpp"
#include "unittest/gtest.hpp"
#include "unittest/unittest_utils.hpp"

namespace unittest {

namespace {

template <class T>
T round_trip(const T &in) {
    write_message_t wm;
    serialize<cluster_version_t::LATEST_DISK>(&wm, in);
    vector_stream_t stream;
    EXPECT_EQ(0, send_write_message(&stream, &wm));
    std::vector<char> data = stream.vector();
    vector_read_stream_t rs(std::move(data));
    T out;
    EXPECT_EQ(archive_result_t::SUCCESS,
              deserialize<cluster_version_t::LATEST_DISK>(&rs, &out));
    return out;
}

partition_entry_t make_entry(const char *name,
                             partition_state_t state = partition_state_t::ACTIVE) {
    partition_entry_t e;
    e.id = generate_uuid();
    e.name = name_string_t::guarantee_valid(name);
    e.storage_id = generate_uuid();
    e.state = state;
    e.list_default = false;
    return e;
}

/* Two-way range: [minval, 100) and [100, maxval). */
partition_config_t make_valid_range_config() {
    partition_config_t cfg;
    cfg.type = partition_type_t::RANGE;
    cfg.key_field = "ts";
    cfg.epoch = 1;
    cfg.partitions.push_back(make_entry("p_lo"));
    cfg.partitions.push_back(make_entry("p_hi"));
    cfg.range_boundaries.push_back(ql::datum_t::minval());
    cfg.range_boundaries.push_back(ql::datum_t(100.0));
    cfg.range_boundaries.push_back(ql::datum_t::maxval());
    cfg.hash_modulus = 0;
    return cfg;
}

/* Hash modulus 4: one partition per bucket. */
partition_config_t make_valid_hash_config() {
    partition_config_t cfg;
    cfg.type = partition_type_t::HASH;
    cfg.key_field = "user_id";
    cfg.epoch = 2;
    cfg.hash_modulus = 4;
    for (uint32_t b = 0; b < 4; ++b) {
        const std::string name = "h" + std::to_string(b);
        partition_entry_t e = make_entry(name.c_str());
        e.hash_buckets.push_back(b);
        cfg.partitions.push_back(std::move(e));
    }
    return cfg;
}

/* Hash modulus 4 with grouped buckets: even / odd. */
partition_config_t make_grouped_hash_config() {
    partition_config_t cfg;
    cfg.type = partition_type_t::HASH;
    cfg.key_field = "user_id";
    cfg.epoch = 3;
    cfg.hash_modulus = 4;
    partition_entry_t even = make_entry("even");
    even.hash_buckets = {0, 2};
    partition_entry_t odd = make_entry("odd");
    odd.hash_buckets = {1, 3};
    cfg.partitions.push_back(std::move(even));
    cfg.partitions.push_back(std::move(odd));
    return cfg;
}

/* List: "us" and "eu" explicit; default catches the rest. */
partition_config_t make_valid_list_config() {
    partition_config_t cfg;
    cfg.type = partition_type_t::LIST;
    cfg.key_field = "region";
    cfg.epoch = 4;
    partition_entry_t us = make_entry("us");
    us.list_values.push_back(ql::datum_t(datum_string_t("us")));
    partition_entry_t eu = make_entry("eu");
    eu.list_values.push_back(ql::datum_t(datum_string_t("eu")));
    partition_entry_t other = make_entry("other");
    other.list_default = true;
    cfg.partitions.push_back(std::move(us));
    cfg.partitions.push_back(std::move(eu));
    cfg.partitions.push_back(std::move(other));
    return cfg;
}

void expect_validate_throws(const partition_config_t &cfg) {
    try {
        cfg.validate_or_throw();
        FAIL() << "expected validate_or_throw to throw";
    } catch (const ql::datum_exc_t &) {
        /* expected */
    } catch (const ql::base_exc_t &) {
        /* expected (any datum/runtime fail path) */
    }
}

}  // namespace

/* ── 1. Serialization round trips ────────────────────────────────────────── */

TEST(PartitionConfigTest, EnumSerializationRoundTrip) {
    for (partition_type_t t : {
             partition_type_t::NONE,
             partition_type_t::RANGE,
             partition_type_t::HASH,
             partition_type_t::LIST}) {
        EXPECT_EQ(t, round_trip(t));
    }
    for (partition_state_t s : {
             partition_state_t::CREATING,
             partition_state_t::CATCHING_UP,
             partition_state_t::ACTIVE,
             partition_state_t::DRAINING,
             partition_state_t::FAILED}) {
        EXPECT_EQ(s, round_trip(s));
    }
}

TEST(PartitionConfigTest, EntrySerializationRoundTrip) {
    partition_entry_t e = make_entry("p0", partition_state_t::DRAINING);
    e.primary_key_range = key_range_t::universe();
    e.hash_buckets = {1, 3, 5};
    e.list_values.push_back(ql::datum_t(42.0));
    e.list_values.push_back(ql::datum_t(datum_string_t("x")));
    e.list_default = true;

    partition_entry_t out = round_trip(e);
    EXPECT_EQ(e, out);
    EXPECT_EQ(e.id, out.id);
    EXPECT_EQ(e.name.str(), out.name.str());
    EXPECT_EQ(e.storage_id, out.storage_id);
    EXPECT_EQ(partition_state_t::DRAINING, out.state);
    EXPECT_EQ(key_range_t::universe(), out.primary_key_range);
    EXPECT_EQ(e.hash_buckets, out.hash_buckets);
    ASSERT_EQ(2u, out.list_values.size());
    EXPECT_EQ(e.list_values[0], out.list_values[0]);
    EXPECT_EQ(e.list_values[1], out.list_values[1]);
    EXPECT_TRUE(out.list_default);
}

TEST(PartitionConfigTest, ConfigSerializationRoundTripRange) {
    partition_config_t cfg = make_valid_range_config();
    cfg.validate_or_throw();
    partition_config_t out = round_trip(cfg);
    EXPECT_EQ(cfg, out);
    EXPECT_EQ(partition_type_t::RANGE, out.type);
    EXPECT_EQ("ts", out.key_field);
    EXPECT_EQ(1u, out.epoch);
    ASSERT_EQ(2u, out.partitions.size());
    ASSERT_EQ(3u, out.range_boundaries.size());
    EXPECT_EQ(ql::datum_t::minval(), out.range_boundaries[0]);
    EXPECT_EQ(ql::datum_t(100.0), out.range_boundaries[1]);
    EXPECT_EQ(ql::datum_t::maxval(), out.range_boundaries[2]);
}

TEST(PartitionConfigTest, ConfigSerializationRoundTripHash) {
    partition_config_t cfg = make_valid_hash_config();
    cfg.validate_or_throw();
    EXPECT_EQ(cfg, round_trip(cfg));
}

TEST(PartitionConfigTest, ConfigSerializationRoundTripList) {
    partition_config_t cfg = make_valid_list_config();
    cfg.validate_or_throw();
    EXPECT_EQ(cfg, round_trip(cfg));
}

TEST(PartitionConfigTest, CatalogBlobSerializationRoundTrip) {
    partition_catalog_t cat;
    cat.format_version = PARTITION_CATALOG_FORMAT_VERSION;
    cat.epoch = 7;
    cat.primary_key_directory_block = NULL_BLOCK_ID;
    cat.transition_active = true;
    cat.transition_source_epoch = 6;
    cat.next_mutation_stamp = 3;
    cat.high_water_mark = 2;

    partition_store_ref_t s;
    s.partition_id = generate_uuid();
    s.storage_id = generate_uuid();
    s.shard_superblocks = {NULL_BLOCK_ID, NULL_BLOCK_ID};
    s.state = partition_state_t::ACTIVE;
    s.epoch = 7;
    cat.stores.push_back(s);

    transition_modification_t m;
    m.primary_key = store_key_t("pk1");
    m.value = ql::datum_t(1.5);
    m.mutation_stamp = 1;
    cat.transition_queue.push_back(m);

    partition_catalog_t out = round_trip(cat);
    EXPECT_EQ(cat.format_version, out.format_version);
    EXPECT_EQ(cat.epoch, out.epoch);
    EXPECT_EQ(cat.primary_key_directory_block, out.primary_key_directory_block);
    EXPECT_EQ(cat.transition_active, out.transition_active);
    EXPECT_EQ(cat.transition_source_epoch, out.transition_source_epoch);
    EXPECT_EQ(cat.next_mutation_stamp, out.next_mutation_stamp);
    EXPECT_EQ(cat.high_water_mark, out.high_water_mark);
    ASSERT_EQ(1u, out.stores.size());
    EXPECT_EQ(s.partition_id, out.stores[0].partition_id);
    EXPECT_EQ(s.storage_id, out.stores[0].storage_id);
    EXPECT_EQ(partition_state_t::ACTIVE, out.stores[0].state);
    ASSERT_EQ(1u, out.transition_queue.size());
    EXPECT_EQ(store_key_t("pk1"), out.transition_queue[0].primary_key);
    EXPECT_EQ(1.5, out.transition_queue[0].value.as_num());
}

TEST(PartitionConfigTest, DefaultConfigIsNone) {
    partition_config_t cfg;
    EXPECT_EQ(partition_type_t::NONE, cfg.type);
    EXPECT_TRUE(cfg.key_field.empty());
    EXPECT_EQ(0u, cfg.epoch);
    EXPECT_TRUE(cfg.partitions.empty());
    EXPECT_TRUE(cfg.range_boundaries.empty());
    EXPECT_EQ(0u, cfg.hash_modulus);
    EXPECT_FALSE(cfg.is_partitioned());
    cfg.validate_or_throw();  /* NONE is valid when empty */
}

/* Truncated config tail must produce an error, not a default. */
TEST(PartitionConfigTest, TruncatedConfigTailFails) {
    partition_config_t cfg = make_valid_range_config();
    write_message_t wm;
    serialize<cluster_version_t::LATEST_DISK>(&wm, cfg);
    vector_stream_t stream;
    ASSERT_EQ(0, send_write_message(&stream, &wm));
    std::vector<char> data = stream.vector();
    ASSERT_GT(data.size(), 4u);
    data.resize(data.size() / 2);  /* truncate mid-payload */
    vector_read_stream_t rs(std::move(data));
    partition_config_t out;
    archive_result_t res =
        deserialize<cluster_version_t::LATEST_DISK>(&rs, &out);
    EXPECT_TRUE(bad(res));
}

/* ── 2. Range validation ─────────────────────────────────────────────────── */

TEST(PartitionConfigTest, RangeValidSinglePartition) {
    partition_config_t cfg;
    cfg.type = partition_type_t::RANGE;
    cfg.key_field = "x";
    cfg.epoch = 1;
    cfg.partitions.push_back(make_entry("all"));
    cfg.range_boundaries.push_back(ql::datum_t::minval());
    cfg.range_boundaries.push_back(ql::datum_t::maxval());
    cfg.validate_or_throw();
}

TEST(PartitionConfigTest, RangeValidMultipleAdjacent) {
    partition_config_t cfg = make_valid_range_config();
    /* Add a middle range: [minval, 0), [0, 100), [100, maxval). */
    partition_entry_t mid = make_entry("p_mid");
    cfg.partitions.insert(cfg.partitions.begin() + 1, mid);
    cfg.range_boundaries.clear();
    cfg.range_boundaries.push_back(ql::datum_t::minval());
    cfg.range_boundaries.push_back(ql::datum_t(0.0));
    cfg.range_boundaries.push_back(ql::datum_t(100.0));
    cfg.range_boundaries.push_back(ql::datum_t::maxval());
    cfg.validate_or_throw();
}

TEST(PartitionConfigTest, RangeValidFullCoverage) {
    partition_config_t cfg = make_valid_range_config();
    cfg.validate_or_throw();
    EXPECT_TRUE(cfg.is_partitioned());
}

TEST(PartitionConfigTest, RangeRejectsGapViaMissingBoundary) {
    /* N partitions with fewer than N+1 boundaries. */
    partition_config_t cfg = make_valid_range_config();
    cfg.range_boundaries.pop_back();  /* now 2 bounds for 2 partitions */
    expect_validate_throws(cfg);
}

TEST(PartitionConfigTest, RangeRejectsOverlapViaUnsortedBoundaries) {
    partition_config_t cfg = make_valid_range_config();
    /* Swap so boundaries are not strictly increasing: minval, maxval, 100. */
    cfg.range_boundaries[1] = ql::datum_t::maxval();
    cfg.range_boundaries[2] = ql::datum_t(100.0);
    expect_validate_throws(cfg);
}

TEST(PartitionConfigTest, RangeRejectsEqualAdjacentBoundaries) {
    partition_config_t cfg = make_valid_range_config();
    cfg.range_boundaries[1] = ql::datum_t(100.0);
    cfg.range_boundaries[2] = ql::datum_t(100.0);  /* empty interval */
    /* Still need maxval as last — replace with equal midpoints */
    cfg.range_boundaries.clear();
    cfg.range_boundaries.push_back(ql::datum_t::minval());
    cfg.range_boundaries.push_back(ql::datum_t(50.0));
    cfg.range_boundaries.push_back(ql::datum_t(50.0));  /* not strictly sorted */
    expect_validate_throws(cfg);
}

TEST(PartitionConfigTest, RangeRejectsDuplicateNames) {
    partition_config_t cfg = make_valid_range_config();
    cfg.partitions[1].name = cfg.partitions[0].name;
    expect_validate_throws(cfg);
}

TEST(PartitionConfigTest, RangeRejectsInvalidMinPlacement) {
    partition_config_t cfg = make_valid_range_config();
    cfg.range_boundaries[0] = ql::datum_t(0.0);  /* must start at minval */
    expect_validate_throws(cfg);
}

TEST(PartitionConfigTest, RangeRejectsInvalidMaxPlacement) {
    partition_config_t cfg = make_valid_range_config();
    cfg.range_boundaries.back() = ql::datum_t(999.0);  /* must end at maxval */
    expect_validate_throws(cfg);
}

TEST(PartitionConfigTest, RangeRejectsMissingBoundaries) {
    partition_config_t cfg;
    cfg.type = partition_type_t::RANGE;
    cfg.key_field = "ts";
    cfg.partitions.push_back(make_entry("p0"));
    /* range_boundaries left empty */
    expect_validate_throws(cfg);
}

TEST(PartitionConfigTest, RangeRejectsEmptyPartitions) {
    partition_config_t cfg;
    cfg.type = partition_type_t::RANGE;
    cfg.key_field = "ts";
    cfg.range_boundaries.push_back(ql::datum_t::minval());
    cfg.range_boundaries.push_back(ql::datum_t::maxval());
    expect_validate_throws(cfg);
}

TEST(PartitionConfigTest, RangeRejectsEmptyKeyField) {
    partition_config_t cfg = make_valid_range_config();
    cfg.key_field.clear();
    expect_validate_throws(cfg);
}

/* ── 3. Hash validation ──────────────────────────────────────────────────── */

TEST(PartitionConfigTest, HashValidOneBucketPerPartition) {
    partition_config_t cfg = make_valid_hash_config();
    cfg.validate_or_throw();
}

TEST(PartitionConfigTest, HashValidGroupedBuckets) {
    partition_config_t cfg = make_grouped_hash_config();
    cfg.validate_or_throw();
}

TEST(PartitionConfigTest, HashRejectsDuplicateBuckets) {
    partition_config_t cfg = make_valid_hash_config();
    cfg.partitions[0].hash_buckets.push_back(1);  /* bucket 1 already owned */
    expect_validate_throws(cfg);
}

TEST(PartitionConfigTest, HashRejectsOmittedBuckets) {
    partition_config_t cfg = make_valid_hash_config();
    cfg.partitions[3].hash_buckets.clear();  /* drop coverage of bucket 3 */
    expect_validate_throws(cfg);
}

TEST(PartitionConfigTest, HashRejectsOutOfRangeBuckets) {
    partition_config_t cfg = make_valid_hash_config();
    cfg.partitions[0].hash_buckets[0] = 99;  /* >= modulus 4 */
    expect_validate_throws(cfg);
}

TEST(PartitionConfigTest, HashRejectsBucketAtModulus) {
    partition_config_t cfg = make_valid_hash_config();
    cfg.partitions[0].hash_buckets.clear();
    cfg.partitions[0].hash_buckets.push_back(cfg.hash_modulus);  /* == modulus */
    expect_validate_throws(cfg);
}

TEST(PartitionConfigTest, HashRejectsMissingModulus) {
    partition_config_t cfg = make_valid_hash_config();
    cfg.hash_modulus = 0;
    expect_validate_throws(cfg);
}

TEST(PartitionConfigTest, HashRejectsModulusTooSmall) {
    partition_config_t cfg = make_valid_hash_config();
    cfg.hash_modulus = 1;  /* below PARTITION_HASH_MODULUS_MIN */
    expect_validate_throws(cfg);
}

TEST(PartitionConfigTest, HashRejectsModulusTooLarge) {
    partition_config_t cfg;
    cfg.type = partition_type_t::HASH;
    cfg.key_field = "k";
    cfg.hash_modulus = PARTITION_HASH_MODULUS_MAX + 1;
    partition_entry_t e = make_entry("p0");
    e.hash_buckets.push_back(0);
    cfg.partitions.push_back(e);
    expect_validate_throws(cfg);
}

TEST(PartitionConfigTest, HashRejectsNonemptyRangeBoundaries) {
    partition_config_t cfg = make_valid_hash_config();
    cfg.range_boundaries.push_back(ql::datum_t::minval());
    expect_validate_throws(cfg);
}

/* ── 4. List validation ──────────────────────────────────────────────────── */

TEST(PartitionConfigTest, ListValidExplicitAndDefault) {
    partition_config_t cfg = make_valid_list_config();
    cfg.validate_or_throw();
}

TEST(PartitionConfigTest, ListRejectsDuplicateValues) {
    partition_config_t cfg = make_valid_list_config();
    cfg.partitions[1].list_values.push_back(
        ql::datum_t(datum_string_t("us")));  /* already in p0 */
    expect_validate_throws(cfg);
}

TEST(PartitionConfigTest, ListRejectsDuplicateDefaults) {
    partition_config_t cfg = make_valid_list_config();
    cfg.partitions[0].list_default = true;  /* second default */
    expect_validate_throws(cfg);
}

TEST(PartitionConfigTest, ListRejectsAbsentDefault) {
    partition_config_t cfg = make_valid_list_config();
    cfg.partitions[2].list_default = false;
    expect_validate_throws(cfg);
}

TEST(PartitionConfigTest, ListRejectsNullValue) {
    partition_config_t cfg = make_valid_list_config();
    cfg.partitions[0].list_values.push_back(ql::datum_t::null());
    expect_validate_throws(cfg);
}

TEST(PartitionConfigTest, ListRejectsArrayValue) {
    partition_config_t cfg = make_valid_list_config();
    ql::datum_array_builder_t arr(ql::configured_limits_t::unlimited);
    arr.add(ql::datum_t(1.0));
    cfg.partitions[0].list_values.push_back(std::move(arr).to_datum());
    expect_validate_throws(cfg);
}

TEST(PartitionConfigTest, ListRejectsObjectValue) {
    partition_config_t cfg = make_valid_list_config();
    ql::datum_object_builder_t obj;
    obj.overwrite("a", ql::datum_t(1.0));
    cfg.partitions[0].list_values.push_back(std::move(obj).to_datum());
    expect_validate_throws(cfg);
}

TEST(PartitionConfigTest, ListRejectsMinvalMaxval) {
    partition_config_t cfg = make_valid_list_config();
    cfg.partitions[0].list_values.push_back(ql::datum_t::minval());
    expect_validate_throws(cfg);

    partition_config_t cfg2 = make_valid_list_config();
    cfg2.partitions[0].list_values.push_back(ql::datum_t::maxval());
    expect_validate_throws(cfg2);
}

TEST(PartitionConfigTest, ListAcceptsNumericAndBoolScalars) {
    partition_config_t cfg;
    cfg.type = partition_type_t::LIST;
    cfg.key_field = "flag";
    partition_entry_t a = make_entry("a");
    a.list_values.push_back(ql::datum_t(1.0));
    a.list_values.push_back(ql::datum_t::boolean(true));
    partition_entry_t def = make_entry("def");
    def.list_default = true;
    cfg.partitions.push_back(std::move(a));
    cfg.partitions.push_back(std::move(def));
    cfg.validate_or_throw();
}

/* ── 5. Routing ──────────────────────────────────────────────────────────── */

TEST(PartitionConfigTest, RouteRangeWithinBoundary) {
    partition_config_t cfg = make_valid_range_config();
    cfg.validate_or_throw();

    const partition_entry_t *lo = cfg.route(ql::datum_t(50.0));
    ASSERT_NE(nullptr, lo);
    EXPECT_EQ("p_lo", lo->name.str());

    const partition_entry_t *hi = cfg.route(ql::datum_t(150.0));
    ASSERT_NE(nullptr, hi);
    EXPECT_EQ("p_hi", hi->name.str());
}

TEST(PartitionConfigTest, RouteRangeAtBoundary) {
    partition_config_t cfg = make_valid_range_config();
    /* Half-open [minval, 100) / [100, maxval): key == 100 goes to p_hi. */
    const partition_entry_t *at = cfg.route(ql::datum_t(100.0));
    ASSERT_NE(nullptr, at);
    EXPECT_EQ("p_hi", at->name.str());

    /* Just below boundary stays in p_lo. */
    const partition_entry_t *below = cfg.route(ql::datum_t(99.9));
    ASSERT_NE(nullptr, below);
    EXPECT_EQ("p_lo", below->name.str());
}

TEST(PartitionConfigTest, RouteRangeMinvalMaxval) {
    partition_config_t cfg = make_valid_range_config();
    /* minval is a legal left edge of the first interval. */
    const partition_entry_t *mn = cfg.route(ql::datum_t::minval());
    ASSERT_NE(nullptr, mn);
    EXPECT_EQ("p_lo", mn->name.str());

    /* maxval is the exclusive right edge of the last interval → no owner. */
    EXPECT_EQ(nullptr, cfg.route(ql::datum_t::maxval()));
}

TEST(PartitionConfigTest, RouteHashKnownVectors) {
    partition_config_t cfg = make_valid_hash_config();
    cfg.validate_or_throw();

    const ql::datum_t keys[] = {
        ql::datum_t(datum_string_t("alice")),
        ql::datum_t(datum_string_t("bob")),
        ql::datum_t(1.0),
        ql::datum_t(2.0),
        ql::datum_t::boolean(true),
        ql::datum_t::boolean(false),
    };

    std::set<uint32_t> seen_buckets;
    for (const ql::datum_t &k : keys) {
        const uint32_t bucket = partition_hash_bucket(k, cfg.hash_modulus);
        seen_buckets.insert(bucket);
        const partition_entry_t *p = cfg.route(k);
        ASSERT_NE(nullptr, p) << "key should route to a partition";
        ASSERT_EQ(1u, p->hash_buckets.size());
        EXPECT_EQ(bucket, p->hash_buckets[0]);
    }
    /* Hash is deterministic across calls. */
    for (const ql::datum_t &k : keys) {
        EXPECT_EQ(partition_hash_bucket(k, cfg.hash_modulus),
                  partition_hash_bucket(k, cfg.hash_modulus));
    }
    (void)seen_buckets;
}

TEST(PartitionConfigTest, RouteHashEveryBucketCovered) {
    partition_config_t cfg = make_grouped_hash_config();
    cfg.validate_or_throw();

    /* Probe until every bucket has been hit, then confirm route matches group. */
    std::vector<bool> hit(cfg.hash_modulus, false);
    size_t covered = 0;
    for (int i = 0; i < 10000 && covered < cfg.hash_modulus; ++i) {
        ql::datum_t k(datum_string_t(std::to_string(i)));
        uint32_t b = partition_hash_bucket(k, cfg.hash_modulus);
        if (!hit[b]) {
            hit[b] = true;
            ++covered;
        }
        const partition_entry_t *p = cfg.route(k);
        ASSERT_NE(nullptr, p);
        bool owns = false;
        for (uint32_t pb : p->hash_buckets) {
            if (pb == b) {
                owns = true;
                break;
            }
        }
        EXPECT_TRUE(owns);
        if (b % 2 == 0) {
            EXPECT_EQ("even", p->name.str());
        } else {
            EXPECT_EQ("odd", p->name.str());
        }
    }
    EXPECT_EQ(static_cast<size_t>(cfg.hash_modulus), covered);
}

TEST(PartitionConfigTest, RouteListExplicitMatch) {
    partition_config_t cfg = make_valid_list_config();
    cfg.validate_or_throw();

    const partition_entry_t *us =
        cfg.route(ql::datum_t(datum_string_t("us")));
    ASSERT_NE(nullptr, us);
    EXPECT_EQ("us", us->name.str());

    const partition_entry_t *eu =
        cfg.route(ql::datum_t(datum_string_t("eu")));
    ASSERT_NE(nullptr, eu);
    EXPECT_EQ("eu", eu->name.str());
}

TEST(PartitionConfigTest, RouteListDefaultMatch) {
    partition_config_t cfg = make_valid_list_config();
    const partition_entry_t *other =
        cfg.route(ql::datum_t(datum_string_t("apac")));
    ASSERT_NE(nullptr, other);
    EXPECT_EQ("other", other->name.str());
    EXPECT_TRUE(other->list_default);
}

TEST(PartitionConfigTest, RouteListNoMatchWithoutDefault) {
    /* Build an invalid config without default only for route() behavior —
     * route falls through to nullptr when no default is present. */
    partition_config_t cfg;
    cfg.type = partition_type_t::LIST;
    cfg.key_field = "region";
    partition_entry_t us = make_entry("us");
    us.list_values.push_back(ql::datum_t(datum_string_t("us")));
    cfg.partitions.push_back(std::move(us));
    /* no list_default */
    EXPECT_EQ(nullptr, cfg.route(ql::datum_t(datum_string_t("eu"))));
}

TEST(PartitionConfigTest, RouteNoneReturnsNull) {
    partition_config_t cfg;
    EXPECT_EQ(nullptr, cfg.route(ql::datum_t(1.0)));
}

TEST(PartitionConfigTest, RouteExactlyOnceProperty) {
    /* Every legal probe key routes to exactly one partition (non-null). */
    partition_config_t range = make_valid_range_config();
    range.validate_or_throw();
    for (double v = -50.0; v < 250.0; v += 10.0) {
        EXPECT_NE(nullptr, range.route(ql::datum_t(v)));
    }

    partition_config_t hash = make_valid_hash_config();
    hash.validate_or_throw();
    for (int i = 0; i < 50; ++i) {
        EXPECT_NE(nullptr, hash.route(ql::datum_t(static_cast<double>(i))));
    }

    partition_config_t list = make_valid_list_config();
    list.validate_or_throw();
    for (const char *s : {"us", "eu", "apac", "sa", ""}) {
        EXPECT_NE(nullptr, list.route(ql::datum_t(datum_string_t(s))));
    }
}

TEST(PartitionConfigTest, PartitionMapRoutesFor) {
    partition_map_t map;
    map.epoch = 5;
    uuid_u p1 = generate_uuid();
    uuid_u p2 = generate_uuid();
    namespace_id_t s1 = generate_uuid();
    namespace_id_t s2 = generate_uuid();
    map.stores[p1] = s1;
    map.stores[p2] = s2;
    map.primary_ranges[p1] = key_range_t::universe();
    map.primary_ranges[p2] = key_range_t::universe();

    partition_selection_t sel;
    sel.partition_ids.push_back(p1);
    sel.partition_ids.push_back(p2);
    sel.partition_ids.push_back(generate_uuid());  /* unknown — skipped */

    std::vector<partition_route_t> routes = map.routes_for(sel);
    ASSERT_EQ(2u, routes.size());
    EXPECT_EQ(p1, routes[0].partition_id);
    EXPECT_EQ(s1, routes[0].storage_id);
    EXPECT_EQ(5u, routes[0].epoch);
    EXPECT_EQ(p2, routes[1].partition_id);
    EXPECT_EQ(s2, routes[1].storage_id);
}

TEST(PartitionConfigTest, PruneEqualityNeverFalseNegative) {
    partition_config_t cfg = make_valid_range_config();
    for (partition_entry_t &p : cfg.partitions) {
        p.state = partition_state_t::ACTIVE;
    }
    cfg.validate_or_throw();

    ql::datum_t key(50.0);
    const partition_entry_t *expected = cfg.route(key);
    ASSERT_NE(nullptr, expected);

    auto selected = cfg.prune(
        partition_predicate_t::make_equality("ts", key));
    ASSERT_EQ(1u, selected.size());
    EXPECT_EQ(expected->id, selected[0]->id);

    /* Field mismatch must fall back to all ACTIVE. */
    auto all = cfg.prune(
        partition_predicate_t::make_equality("other", key));
    EXPECT_EQ(2u, all.size());
}

}  // namespace unittest
