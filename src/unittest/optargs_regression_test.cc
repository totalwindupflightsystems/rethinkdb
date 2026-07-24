// Copyright 2026 RethinkDB, all rights reserved.
#include "unittest/gtest.hpp"

#include "rdb_protocol/optargs.hpp"

namespace unittest {

// REGRESSION: CDC/vector/FTS/BRIN features added new optargs
// (partitions, fts, vector, brin) but forgot to register them
// in global_optargs_t::acceptable_optargs. The server crashed
// on every table_create() and index_create() query until fixed.
// These tests guarantee that oversight can never regress.

TEST(OptargRegressionTest, PartitionsOptargIsValid) {
    EXPECT_TRUE(ql::global_optargs_t::optarg_is_valid("partitions"));
}

TEST(OptargRegressionTest, FtsOptargIsValid) {
    EXPECT_TRUE(ql::global_optargs_t::optarg_is_valid("fts"));
}

TEST(OptargRegressionTest, VectorOptargIsValid) {
    EXPECT_TRUE(ql::global_optargs_t::optarg_is_valid("vector"));
}

TEST(OptargRegressionTest, BrinOptargIsValid) {
    EXPECT_TRUE(ql::global_optargs_t::optarg_is_valid("brin"));
}

TEST(OptargRegressionTest, AllUpstreamOptargsStillValid) {
    // Verify we didn't accidentally break any original optargs
    EXPECT_TRUE(ql::global_optargs_t::optarg_is_valid("primary_key"));
    EXPECT_TRUE(ql::global_optargs_t::optarg_is_valid("durability"));
    EXPECT_TRUE(ql::global_optargs_t::optarg_is_valid("shards"));
    EXPECT_TRUE(ql::global_optargs_t::optarg_is_valid("replicas"));
    EXPECT_TRUE(ql::global_optargs_t::optarg_is_valid("conflict"));
    EXPECT_TRUE(ql::global_optargs_t::optarg_is_valid("index"));
    EXPECT_TRUE(ql::global_optargs_t::optarg_is_valid("noreply"));
    EXPECT_TRUE(ql::global_optargs_t::optarg_is_valid("return_changes"));
}

TEST(OptargRegressionTest, BogusOptargRejected) {
    EXPECT_FALSE(ql::global_optargs_t::optarg_is_valid("nonexistent_optarg_xyz"));
    EXPECT_FALSE(ql::global_optargs_t::optarg_is_valid(""));
}

}  // namespace unittest
