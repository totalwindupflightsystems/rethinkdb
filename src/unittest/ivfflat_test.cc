// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/ivfflat.hpp"

#include <cmath>
#include <vector>

#include "containers/archive/vector_stream.hpp"
#include "unittest/gtest.hpp"

namespace unittest {

constexpr double kEpsilon = 1e-12;

// ── Construction ──

TEST(IVFFlatTest, EmptyConstruction) {
    ql::ivfflat_index_t idx(10, 5, 3, ql::ivfflat_index_t::metric_t::L2);
    EXPECT_FALSE(idx.is_trained());
    EXPECT_EQ(0u, idx.size());
    EXPECT_EQ(10, idx.get_nlist());
    EXPECT_EQ(5, idx.get_nprobe());
    EXPECT_EQ(3u, idx.get_dim());
}

// ── Training ──

TEST(IVFFlatTest, TrainBasic) {
    ql::ivfflat_index_t idx(5, 3, 2, ql::ivfflat_index_t::metric_t::L2);

    // 10 2D vectors clustered around two points
    std::vector<std::vector<double>> data = {
        {1.0, 1.0}, {1.1, 0.9}, {0.9, 1.1}, {1.0, 0.8}, {1.2, 1.0},
        {5.0, 5.0}, {4.9, 5.1}, {5.1, 4.9}, {5.0, 4.8}, {5.2, 5.0}
    };
    idx.train(data, 20);
    EXPECT_TRUE(idx.is_trained());
    EXPECT_EQ(5u, idx.get_centroids().size());
}

TEST(IVFFlatTest, TrainSingleCluster) {
    ql::ivfflat_index_t idx(1, 1, 3, ql::ivfflat_index_t::metric_t::L2);

    std::vector<std::vector<double>> data = {
        {1.0, 2.0, 3.0}, {1.1, 2.1, 3.1}, {0.9, 1.9, 2.9}
    };
    idx.train(data, 20);
    EXPECT_TRUE(idx.is_trained());
    EXPECT_EQ(1u, idx.get_centroids().size());

    // Single centroid should be near the mean: ~{1.0, 2.0, 3.0}
    const auto &centroids = idx.get_centroids();
    EXPECT_NEAR(1.0, centroids[0][0], 0.5);
    EXPECT_NEAR(2.0, centroids[0][1], 0.5);
    EXPECT_NEAR(3.0, centroids[0][2], 0.5);
}

// ── Insert ──

TEST(IVFFlatTest, SingleInsert) {
    ql::ivfflat_index_t idx(3, 2, 2, ql::ivfflat_index_t::metric_t::L2);

    std::vector<std::vector<double>> data = {
        {1.0, 1.0}, {5.0, 5.0}, {1.5, 1.5}
    };
    idx.train(data, 20);

    store_key_t key(std::string("doc1"));
    double v[] = {1.0, 2.0};
    idx.insert(v, key);

    EXPECT_EQ(1u, idx.size());
}

TEST(IVFFlatTest, MultipleInserts) {
    ql::ivfflat_index_t idx(3, 2, 2, ql::ivfflat_index_t::metric_t::L2);

    std::vector<std::vector<double>> data = {
        {1.0, 1.0}, {5.0, 5.0}, {9.0, 9.0}
    };
    idx.train(data, 20);

    store_key_t k1(std::string("a"));
    store_key_t k2(std::string("b"));
    store_key_t k3(std::string("c"));

    double v1[] = {1.0, 1.0};
    double v2[] = {5.0, 5.0};
    double v3[] = {9.0, 9.0};

    idx.insert(v1, k1);
    idx.insert(v2, k2);
    idx.insert(v3, k3);

    EXPECT_EQ(3u, idx.size());
}

// ── Search ──

TEST(IVFFlatTest, EmptySearch) {
    ql::ivfflat_index_t idx(3, 2, 3, ql::ivfflat_index_t::metric_t::L2);

    std::vector<std::vector<double>> data = {
        {1.0, 1.0, 1.0}, {5.0, 5.0, 5.0}, {9.0, 9.0, 9.0}
    };
    idx.train(data, 20);

    double query[] = {1.0, 0.0, 0.0};
    auto results = idx.search_knn(query, 3);
    EXPECT_TRUE(results.empty());
}

TEST(IVFFlatTest, SingleInsertSearch) {
    ql::ivfflat_index_t idx(3, 2, 2, ql::ivfflat_index_t::metric_t::L2);

    std::vector<std::vector<double>> data = {
        {1.0, 1.0}, {5.0, 5.0}, {9.0, 9.0}
    };
    idx.train(data, 20);

    store_key_t key(std::string("doc1"));
    double v[] = {1.0, 2.0};
    idx.insert(v, key);

    // Search for the same vector
    auto results = idx.search_knn(v, 1);
    ASSERT_EQ(1u, results.size());
    EXPECT_EQ(key, results[0].second);
    EXPECT_NEAR(0.0, results[0].first, kEpsilon);
}

TEST(IVFFlatTest, MultiInsertSearch) {
    ql::ivfflat_index_t idx(5, 5, 2, ql::ivfflat_index_t::metric_t::L2);

    std::vector<std::vector<double>> data = {
        {1.0, 1.0}, {1.5, 1.5}, {5.0, 5.0}, {5.5, 5.5}, {9.0, 9.0}
    };
    idx.train(data, 20);

    store_key_t k1(std::string("a"));
    store_key_t k2(std::string("b"));
    store_key_t k3(std::string("c"));
    store_key_t k4(std::string("d"));
    store_key_t k5(std::string("e"));

    double v1[] = {1.0, 1.0};
    double v2[] = {2.0, 2.0};
    double v3[] = {5.0, 5.0};
    double v4[] = {6.0, 6.0};
    double v5[] = {9.0, 9.0};

    idx.insert(v1, k1);
    idx.insert(v2, k2);
    idx.insert(v3, k3);
    idx.insert(v4, k4);
    idx.insert(v5, k5);

    // Search near {1.0, 1.0} — should find k1 first, then k2
    double query[] = {1.0, 1.0};
    auto results = idx.search_knn(query, 3);
    ASSERT_EQ(3u, results.size());

    // First result should be k1 (exact match)
    EXPECT_EQ(k1, results[0].second);
    EXPECT_NEAR(0.0, results[0].first, kEpsilon);
}

// ── Remove ──

TEST(IVFFlatTest, RemoveExisting) {
    ql::ivfflat_index_t idx(3, 2, 2, ql::ivfflat_index_t::metric_t::L2);

    std::vector<std::vector<double>> data = {
        {1.0, 1.0}, {5.0, 5.0}, {9.0, 9.0}
    };
    idx.train(data, 20);

    store_key_t k1(std::string("a"));
    store_key_t k2(std::string("b"));

    double v1[] = {1.0, 1.0};
    double v2[] = {5.0, 5.0};

    idx.insert(v1, k1);
    idx.insert(v2, k2);
    EXPECT_EQ(2u, idx.size());

    idx.remove(k1);
    EXPECT_EQ(1u, idx.size());

    // Searching should not return k1
    double query[] = {1.0, 1.0};
    auto results = idx.search_knn(query, 2, 3);
    // Probe all 3 centroids to ensure we don't miss k2
    ASSERT_EQ(1u, results.size());
    EXPECT_EQ(k2, results[0].second);
}

TEST(IVFFlatTest, RemoveNonExisting) {
    ql::ivfflat_index_t idx(3, 2, 2, ql::ivfflat_index_t::metric_t::L2);

    std::vector<std::vector<double>> data = {
        {1.0, 1.0}, {5.0, 5.0}, {9.0, 9.0}
    };
    idx.train(data, 20);

    store_key_t k1(std::string("a"));
    double v1[] = {1.0, 1.0};
    idx.insert(v1, k1);

    store_key_t k_nonexist(std::string("nonexistent"));
    idx.remove(k_nonexist);  // Should not crash or change size
    EXPECT_EQ(1u, idx.size());
}

// ── Distance metrics ──

TEST(IVFFlatTest, CosineDistance) {
    ql::ivfflat_index_t idx(3, 2, 2, ql::ivfflat_index_t::metric_t::COSINE);

    std::vector<std::vector<double>> data = {
        {1.0, 0.0}, {0.0, 1.0}, {1.0, 1.0}
    };
    idx.train(data, 20);

    store_key_t k1(std::string("a"));
    store_key_t k2(std::string("b"));

    double v1[] = {1.0, 0.0};
    double v2[] = {0.0, 1.0};

    idx.insert(v1, k1);
    idx.insert(v2, k2);

    // Query identical to v1
    double query[] = {1.0, 0.0};
    auto results = idx.search_knn(query, 2, 3);
    ASSERT_EQ(2u, results.size());
    EXPECT_EQ(k1, results[0].second);
    EXPECT_NEAR(0.0, results[0].first, kEpsilon);
}

TEST(IVFFlatTest, InnerProductDistance) {
    ql::ivfflat_index_t idx(3, 2, 2,
                             ql::ivfflat_index_t::metric_t::INNER_PRODUCT);

    std::vector<std::vector<double>> data = {
        {1.0, 0.0}, {2.0, 0.0}, {0.5, 0.0}
    };
    idx.train(data, 20);

    store_key_t k1(std::string("a"));
    store_key_t k2(std::string("b"));

    double v1[] = {1.0, 0.0};   // inner product with self = -1.0
    double v2[] = {2.0, 0.0};   // inner product with self = -4.0

    idx.insert(v1, k1);
    idx.insert(v2, k2);

    // Query: [1.0, 0.0] — k1 is exact match (distance -1.0),
    // k2 is [2.0, 0.0] (inner product = 2.0, distance = -2.0)
    // Closest = most negative = k2
    double query[] = {1.0, 0.0};
    auto results = idx.search_knn(query, 2, 3);
    ASSERT_EQ(2u, results.size());

    // k2 distance = -2.0, k1 distance = -1.0
    // Results are sorted ascending (most negative first)
    EXPECT_EQ(k2, results[0].second);
    EXPECT_NEAR(-2.0, results[0].first, kEpsilon);
    EXPECT_EQ(k1, results[1].second);
    EXPECT_NEAR(-1.0, results[1].first, kEpsilon);
}

// ── nprobe parameter ──

TEST(IVFFlatTest, NprobeParameter) {
    ql::ivfflat_index_t idx(10, 2, 2, ql::ivfflat_index_t::metric_t::L2);

    // 10 vectors spread across space
    std::vector<std::vector<double>> data;
    for (int i = 0; i < 10; ++i) {
        data.push_back({static_cast<double>(i),
                         static_cast<double>(i)});
    }
    idx.train(data, 20);

    store_key_t k(std::string("x"));
    double v[] = {5.0, 5.0};
    idx.insert(v, k);

    // nprobe=1: only probes nearest centroid
    auto results_n1 = idx.search_knn(v, 1, 1);
    EXPECT_EQ(1u, results_n1.size());

    // nprobe=10: probes all centroids
    auto results_n10 = idx.search_knn(v, 1, 10);
    EXPECT_EQ(1u, results_n10.size());
    // Both should find the same result
    EXPECT_EQ(results_n1[0].second, results_n10[0].second);
}

// ── Serialization round-trip ──

TEST(IVFFlatTest, SerializationRoundTrip) {
    ql::ivfflat_index_t idx1(5, 3, 2, ql::ivfflat_index_t::metric_t::COSINE);

    std::vector<std::vector<double>> data = {
        {1.0, 1.0}, {2.0, 2.0}, {3.0, 3.0}, {4.0, 4.0}, {5.0, 5.0}
    };
    idx1.train(data, 20);

    store_key_t k1(std::string("a"));
    store_key_t k2(std::string("b"));

    double v1[] = {1.0, 1.0};
    double v2[] = {3.0, 3.0};

    idx1.insert(v1, k1);
    idx1.insert(v2, k2);

    // Serialize
    write_message_t wm;
    serialize<cluster_version_t::LATEST_DISK>(&wm, idx1);
    vector_stream_t stream;
    ASSERT_EQ(0, send_write_message(&stream, &wm));

    // Deserialize
    std::vector<char> serialized_data = stream.vector();
    vector_read_stream_t read_stream(std::move(serialized_data));
    ql::ivfflat_index_t idx2;
    archive_result_t res =
        deserialize<cluster_version_t::LATEST_DISK>(&read_stream, &idx2);
    EXPECT_EQ(archive_result_t::SUCCESS, res);

    // Verify config
    EXPECT_EQ(idx1.get_nlist(), idx2.get_nlist());
    EXPECT_EQ(idx1.get_nprobe(), idx2.get_nprobe());
    EXPECT_EQ(idx1.get_dim(), idx2.get_dim());
    EXPECT_EQ(idx1.get_metric(), idx2.get_metric());
    EXPECT_EQ(idx1.is_trained(), idx2.is_trained());

    // Verify centroids
    const auto &c1 = idx1.get_centroids();
    const auto &c2 = idx2.get_centroids();
    ASSERT_EQ(c1.size(), c2.size());
    for (size_t i = 0; i < c1.size(); ++i) {
        ASSERT_EQ(c1[i].size(), c2[i].size());
        for (size_t j = 0; j < c1[i].size(); ++j) {
            EXPECT_NEAR(c1[i][j], c2[i][j], kEpsilon);
        }
    }

    // Verify search works after deserialization
    store_key_t k3(std::string("c"));
    double v3[] = {2.0, 2.0};
    idx2.insert(v3, k3);

    double query[] = {2.0, 2.0};
    auto results = idx2.search_knn(query, 2, 5);
    ASSERT_EQ(2u, results.size());
}

// ── Retrain ──

TEST(IVFFlatTest, Retrain) {
    ql::ivfflat_index_t idx(3, 2, 2, ql::ivfflat_index_t::metric_t::L2);

    std::vector<std::vector<double>> data1 = {
        {1.0, 1.0}, {5.0, 5.0}, {9.0, 9.0}
    };
    idx.train(data1, 20);

    store_key_t k1(std::string("a"));
    double v1[] = {1.0, 1.0};
    idx.insert(v1, k1);
    EXPECT_EQ(1u, idx.size());

    // Retrain with different data
    std::vector<std::vector<double>> data2 = {
        {0.0, 0.0}, {10.0, 10.0}, {20.0, 20.0}
    };
    idx.train(data2, 20);

    // After retraining, old data is cleared
    EXPECT_EQ(0u, idx.size());

    // New insert works
    store_key_t k2(std::string("b"));
    double v2[] = {0.0, 0.0};
    idx.insert(v2, k2);
    EXPECT_EQ(1u, idx.size());
}

// ── k > available vectors ──

TEST(IVFFlatTest, MoreResultsThanVectors) {
    ql::ivfflat_index_t idx(3, 2, 2, ql::ivfflat_index_t::metric_t::L2);

    std::vector<std::vector<double>> data = {
        {1.0, 1.0}, {5.0, 5.0}, {9.0, 9.0}
    };
    idx.train(data, 20);

    store_key_t k1(std::string("a"));
    double v1[] = {1.0, 1.0};
    idx.insert(v1, k1);

    // Request 5 results but only 1 exists
    double query[] = {1.0, 1.0};
    auto results = idx.search_knn(query, 5, 3);
    ASSERT_EQ(1u, results.size());
}

}  // namespace unittest
