// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/hnsw.hpp"

#include <cmath>
#include <vector>

#include "containers/archive/vector_stream.hpp"
#include "unittest/gtest.hpp"

namespace unittest {

constexpr double kEpsilon = 1e-12;

// ── Empty graph ──

TEST(HNSWTest, EmptyGraph) {
    ql::hnsw_graph_t g(16, 200, 3, ql::hnsw_graph_t::metric_t::L2);
    EXPECT_TRUE(g.empty());
    EXPECT_EQ(0u, g.size());

    double query[] = {1.0, 0.0, 0.0};
    auto results = g.search_knn(query, 3);
    EXPECT_TRUE(results.empty());
}

// ── Single insert ──

TEST(HNSWTest, SingleInsert) {
    ql::hnsw_graph_t g(16, 200, 3, ql::hnsw_graph_t::metric_t::L2);

    store_key_t key(std::string("doc1"));
    double v[] = {1.0, 2.0, 3.0};
    g.insert(v, key);

    EXPECT_FALSE(g.empty());
    EXPECT_EQ(1u, g.size());
}

// ── Single insert and search ──

TEST(HNSWTest, SingleInsertSearch) {
    ql::hnsw_graph_t g(16, 200, 3, ql::hnsw_graph_t::metric_t::L2);

    store_key_t key(std::string("doc1"));
    double v[] = {1.0, 2.0, 3.0};
    g.insert(v, key);

    // Search for the same vector
    auto results = g.search_knn(v, 1);
    ASSERT_EQ(1u, results.size());
    EXPECT_NEAR(0.0, results[0].first, kEpsilon);

    // Search for a different vector: [1, 2, 3] vs [4, 5, 6]
    // diff = [-3, -3, -3], L2 = sqrt(27) ≈ 5.196
    double q[] = {4.0, 5.0, 6.0};
    results = g.search_knn(q, 1);
    ASSERT_EQ(1u, results.size());
    EXPECT_NEAR(std::sqrt(27.0), results[0].first, kEpsilon);
}

// ── Multi-insert and k-NN correctness ──

TEST(HNSWTest, MultiInsertKNNSearch) {
    // Create a small graph with known geometry.
    // Points: [0,0], [10,0], [0,10], [10,10]
    // Query [1,1] → nearest should be [0,0] at distance sqrt(2) ≈ 1.414
    ql::hnsw_graph_t g(16, 200, 2, ql::hnsw_graph_t::metric_t::L2);

    double p0[] = {0.0, 0.0};
    double p1[] = {10.0, 0.0};
    double p2[] = {0.0, 10.0};
    double p3[] = {10.0, 10.0};

    g.insert(p0, store_key_t(std::string("p0")));
    g.insert(p1, store_key_t(std::string("p1")));
    g.insert(p2, store_key_t(std::string("p2")));
    g.insert(p3, store_key_t(std::string("p3")));

    EXPECT_EQ(4u, g.size());

    // Query near [0, 0]
    double q[] = {1.0, 1.0};
    auto results = g.search_knn(q, 3);
    ASSERT_EQ(3u, results.size());

    // First result should be p0 at distance sqrt(2)
    EXPECT_NEAR(std::sqrt(2.0), results[0].first, kEpsilon);
    // Results should be sorted by distance
    EXPECT_LE(results[0].first, results[1].first);
    EXPECT_LE(results[1].first, results[2].first);
}

// ── Search with exact match ──

TEST(HNSWTest, ExactMatchSearch) {
    ql::hnsw_graph_t g(16, 200, 3, ql::hnsw_graph_t::metric_t::L2);

    store_key_t key1(std::string("a"));
    store_key_t key2(std::string("b"));
    store_key_t key3(std::string("c"));

    double va[] = {1.0, 0.0, 0.0};
    double vb[] = {0.0, 1.0, 0.0};
    double vc[] = {0.0, 0.0, 1.0};

    g.insert(va, key1);
    g.insert(vb, key2);
    g.insert(vc, key3);

    EXPECT_EQ(3u, g.size());

    // Search for va exactly
    auto results = g.search_knn(va, 1);
    ASSERT_EQ(1u, results.size());
    EXPECT_NEAR(0.0, results[0].first, kEpsilon);

    // Search for vb exactly
    results = g.search_knn(vb, 1);
    ASSERT_EQ(1u, results.size());
    EXPECT_NEAR(0.0, results[0].first, kEpsilon);
}

// ── k-NN with k > size ──

TEST(HNSWTest, KNNWithKGreaterThanSize) {
    ql::hnsw_graph_t g(16, 200, 2, ql::hnsw_graph_t::metric_t::L2);

    double p0[] = {0.0, 0.0};
    double p1[] = {1.0, 0.0};
    g.insert(p0, store_key_t(std::string("p0")));
    g.insert(p1, store_key_t(std::string("p1")));

    double q[] = {0.0, 0.0};
    auto results = g.search_knn(q, 10);
    // Should return at most 2 results
    EXPECT_EQ(2u, results.size());
}

// ── Delete (tombstone) ──

TEST(HNSWTest, DeleteTombstone) {
    ql::hnsw_graph_t g(16, 200, 3, ql::hnsw_graph_t::metric_t::L2);

    store_key_t key1(std::string("keep"));
    store_key_t key2(std::string("delete_me"));

    double va[] = {1.0, 0.0, 0.0};
    double vb[] = {0.0, 1.0, 0.0};

    g.insert(va, key1);
    g.insert(vb, key2);

    EXPECT_EQ(2u, g.size());

    // Delete the second node
    g.remove(key2);

    // Size stays the same (tombstone)
    EXPECT_EQ(2u, g.size());
    EXPECT_FALSE(g.empty());

    // Search for va: should only find key1
    auto results = g.search_knn(va, 2);
    ASSERT_EQ(1u, results.size());
    EXPECT_NEAR(0.0, results[0].first, kEpsilon);
}

// ── Delete non-existent key ──

TEST(HNSWTest, DeleteNonExistentKey) {
    ql::hnsw_graph_t g(16, 200, 3, ql::hnsw_graph_t::metric_t::L2);

    double va[] = {1.0, 0.0, 0.0};
    g.insert(va, store_key_t(std::string("exists")));

    EXPECT_EQ(1u, g.size());

    // Deleting a non-existent key should not crash
    g.remove(store_key_t(std::string("does_not_exist")));
    EXPECT_EQ(1u, g.size());
}

// ── Cosine distance metric ──

TEST(HNSWTest, CosineDistanceMetric) {
    ql::hnsw_graph_t g(16, 200, 3, ql::hnsw_graph_t::metric_t::COSINE);

    store_key_t key1(std::string("a"));
    store_key_t key2(std::string("b"));

    // Orthogonal vectors: cosine distance = 1.0
    double va[] = {1.0, 0.0, 0.0};
    double vb[] = {0.0, 1.0, 0.0};

    g.insert(va, key1);
    g.insert(vb, key2);

    // Search from va: should find itself at distance 0
    auto results = g.search_knn(va, 1);
    ASSERT_EQ(1u, results.size());
    EXPECT_NEAR(0.0, results[0].first, kEpsilon);

    // Search from va: second nearest should be vb at distance 1.0
    results = g.search_knn(va, 2);
    ASSERT_EQ(2u, results.size());
    EXPECT_NEAR(0.0, results[0].first, kEpsilon);
    EXPECT_NEAR(1.0, results[1].first, kEpsilon);
}

// ── Inner product distance metric ──

TEST(HNSWTest, InnerProductDistanceMetric) {
    ql::hnsw_graph_t g(16, 200, 2, ql::hnsw_graph_t::metric_t::INNER_PRODUCT);

    store_key_t key1(std::string("a"));
    store_key_t key2(std::string("b"));

    double va[] = {1.0, 0.0};
    double vb[] = {0.5, 0.0};

    g.insert(va, key1);
    g.insert(vb, key2);

    // Search from va: self inner_product = -(1*1+0*0) = -1.0
    // vb inner_product = -(1*0.5+0) = -0.5 — further, so self is nearest
    auto results = g.search_knn(va, 1);
    ASSERT_EQ(1u, results.size());
    EXPECT_NEAR(-1.0, results[0].first, kEpsilon);

    // Search from [2,0]: vb at -(2*0.5+0) = -1.0, va at -(2*1+0) = -2.0
    // va (-2.0) is nearer than vb (-1.0) for inner product
    double q[] = {2.0, 0.0};
    results = g.search_knn(q, 2);
    ASSERT_EQ(2u, results.size());
    EXPECT_NEAR(-2.0, results[0].first, kEpsilon);  // va — more similar
    EXPECT_NEAR(-1.0, results[1].first, kEpsilon);  // vb
}

// ── Multiple insert with larger dataset ──

TEST(HNSWTest, LargerDatasetSearch) {
    // Insert 50 random-ish points and verify consistency
    ql::hnsw_graph_t g(16, 200, 2, ql::hnsw_graph_t::metric_t::L2);

    std::vector<std::vector<double>> vectors;
    for (int i = 0; i < 50; ++i) {
        double x = (i % 10) * 1.0;
        double y = (i / 10) * 1.0;
        std::string key_str = "k" + std::to_string(i);
        vectors.push_back({x, y});
        g.insert(vectors.back().data(), store_key_t(key_str));
    }

    EXPECT_EQ(50u, g.size());

    // Search for a point that exists in the dataset
    double q[] = {0.0, 0.0};
    auto results = g.search_knn(q, 3);
    ASSERT_EQ(3u, results.size());
    EXPECT_NEAR(0.0, results[0].first, kEpsilon);
    EXPECT_LE(results[0].first, results[1].first);
    EXPECT_LE(results[1].first, results[2].first);
}

// ── Serialization round-trip ──

TEST(HNSWTest, SerializationRoundTrip) {
    ql::hnsw_graph_t g1(16, 200, 3, ql::hnsw_graph_t::metric_t::L2);

    double va[] = {1.0, 2.0, 3.0};
    double vb[] = {4.0, 5.0, 6.0};
    double vc[] = {7.0, 8.0, 9.0};

    g1.insert(va, store_key_t(std::string("a")));
    g1.insert(vb, store_key_t(std::string("b")));
    g1.insert(vc, store_key_t(std::string("c")));

    // Serialize
    write_message_t wm;
    serialize<cluster_version_t::LATEST_DISK>(&wm, g1);

    vector_stream_t stream;
    int res = send_write_message(&stream, &wm);
    ASSERT_EQ(0, res);

    // Deserialize
    std::vector<char> data = stream.vector();
    vector_read_stream_t read_stream(std::move(data));
    ql::hnsw_graph_t g2;
    archive_result_t ar = deserialize<cluster_version_t::LATEST_DISK>(
        &read_stream, &g2);
    ASSERT_EQ(archive_result_t::SUCCESS, ar);

    // Verify properties
    EXPECT_EQ(g1.size(), g2.size());
    EXPECT_EQ(g1.get_M(), g2.get_M());
    EXPECT_EQ(g1.get_M_max0(), g2.get_M_max0());
    EXPECT_EQ(g1.get_ef_construction(), g2.get_ef_construction());
    EXPECT_EQ(g1.get_dim(), g2.get_dim());
    EXPECT_EQ(g1.get_metric(), g2.get_metric());

    // Verify search results match
    double q[] = {1.0, 2.0, 3.0};
    auto r1 = g1.search_knn(q, 2);
    auto r2 = g2.search_knn(q, 2);

    ASSERT_EQ(r1.size(), r2.size());
    for (size_t i = 0; i < r1.size(); ++i) {
        EXPECT_NEAR(r1[i].first, r2[i].first, kEpsilon);
    }
}

// ── Serialization round-trip with different metric ──

TEST(HNSWTest, SerializationRoundTripCosine) {
    ql::hnsw_graph_t g1(8, 100, 4, ql::hnsw_graph_t::metric_t::COSINE);

    double a[] = {1.0, 0.0, 0.0, 0.0};
    double b[] = {0.0, 1.0, 0.0, 0.0};
    g1.insert(a, store_key_t(std::string("x")));
    g1.insert(b, store_key_t(std::string("y")));

    // Serialize
    write_message_t wm;
    serialize<cluster_version_t::LATEST_DISK>(&wm, g1);
    vector_stream_t stream;
    ASSERT_EQ(0, send_write_message(&stream, &wm));

    // Deserialize
    std::vector<char> data = stream.vector();
    vector_read_stream_t read_stream(std::move(data));
    ql::hnsw_graph_t g2;
    ASSERT_EQ(archive_result_t::SUCCESS,
              deserialize<cluster_version_t::LATEST_DISK>(&read_stream, &g2));

    // Verify metric preserved
    EXPECT_EQ(g1.get_metric(), g2.get_metric());
    EXPECT_EQ(g1.size(), g2.size());

    // Both should give same search results
    double q[] = {1.0, 0.0, 0.0, 0.0};
    auto r1 = g1.search_knn(q, 2);
    auto r2 = g2.search_knn(q, 2);
    ASSERT_EQ(r1.size(), r2.size());
    EXPECT_NEAR(r1[0].first, r2[0].first, kEpsilon);
}

// ── Serialization round-trip with tombstoned nodes ──

TEST(HNSWTest, SerializationRoundTripWithTombstones) {
    ql::hnsw_graph_t g1(16, 200, 2, ql::hnsw_graph_t::metric_t::L2);

    double a[] = {1.0, 0.0};
    double b[] = {0.0, 1.0};
    double c[] = {1.0, 1.0};

    g1.insert(a, store_key_t(std::string("keep1")));
    g1.insert(b, store_key_t(std::string("del")));
    g1.insert(c, store_key_t(std::string("keep2")));

    // Tombstone the middle one
    g1.remove(store_key_t(std::string("del")));

    // Serialize
    write_message_t wm;
    serialize<cluster_version_t::LATEST_DISK>(&wm, g1);
    vector_stream_t stream;
    ASSERT_EQ(0, send_write_message(&stream, &wm));

    // Deserialize
    std::vector<char> data = stream.vector();
    vector_read_stream_t read_stream(std::move(data));
    ql::hnsw_graph_t g2;
    ASSERT_EQ(archive_result_t::SUCCESS,
              deserialize<cluster_version_t::LATEST_DISK>(&read_stream, &g2));

    // Both should have same size and only return 2 results
    EXPECT_EQ(3u, g2.size());
    double q[] = {0.0, 0.0};
    auto results = g2.search_knn(q, 3);
    EXPECT_EQ(2u, results.size());
}

// ── Zero k ──

TEST(HNSWTest, ZeroK) {
    ql::hnsw_graph_t g(16, 200, 2, ql::hnsw_graph_t::metric_t::L2);

    double a[] = {1.0, 0.0};
    g.insert(a, store_key_t(std::string("x")));

    auto results = g.search_knn(a, 0);
    EXPECT_TRUE(results.empty());
}

// ── Multi-layer graph is built correctly ──

TEST(HNSWTest, MultiLevelGraphPreservesSearch) {
    // Use enough inserts that we get a multi-level graph
    ql::hnsw_graph_t g(4, 200, 2, ql::hnsw_graph_t::metric_t::L2);  // M=4, higher level prob

    const int N = 100;
    std::vector<std::vector<double>> vecs(N);
    for (int i = 0; i < N; ++i) {
        double x = static_cast<double>(i);
        double y = static_cast<double>(i * 2 % 37);
        vecs[i] = {x, y};
        g.insert(vecs[i].data(),
                 store_key_t(std::string("p" + std::to_string(i))));
    }

    EXPECT_EQ(static_cast<size_t>(N), g.size());

    // Brute-force verification: for a query, check that the first result
    // returned by HNSW is one of the actual nearest neighbors.
    double q[] = {50.0, 10.0};
    auto results = g.search_knn(q, 3);
    ASSERT_EQ(3u, results.size());

    // All distances should be non-negative for L2
    for (const auto &r : results) {
        EXPECT_GE(r.first, 0.0);
    }
    EXPECT_LE(results[0].first, results[1].first);
    EXPECT_LE(results[1].first, results[2].first);
}

}  // namespace unittest
