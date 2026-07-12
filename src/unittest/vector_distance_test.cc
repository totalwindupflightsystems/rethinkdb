// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/vector_distance.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

#include "unittest/gtest.hpp"

namespace unittest {

constexpr double kEpsilon = 1e-12;

// ── L2 Distance Tests ──

TEST(VectorDistanceTest, L2IdenticalVectors) {
    std::vector<double> v = {1.0, 2.0, 3.0, 4.0};
    double d = ql::l2_distance(v.data(), v.data(), v.size());
    EXPECT_NEAR(0.0, d, kEpsilon);
}

TEST(VectorDistanceTest, L2OrthogonalVectors) {
    // a = [1, 0, 0], b = [0, 1, 0] → distance = sqrt(1+1+0) = sqrt(2)
    std::vector<double> a = {1.0, 0.0, 0.0};
    std::vector<double> b = {0.0, 1.0, 0.0};
    double d = ql::l2_distance(a.data(), b.data(), a.size());
    EXPECT_NEAR(std::sqrt(2.0), d, kEpsilon);
}

TEST(VectorDistanceTest, L2KnownDistance) {
    // a = [0, 0, 0], b = [3, 4, 0] → distance = sqrt(9+16+0) = 5
    std::vector<double> a = {0.0, 0.0, 0.0};
    std::vector<double> b = {3.0, 4.0, 0.0};
    double d = ql::l2_distance(a.data(), b.data(), a.size());
    EXPECT_NEAR(5.0, d, kEpsilon);
}

TEST(VectorDistanceTest, L2NegativeValues) {
    // a = [-1, -2, -3], b = [1, 2, 3]
    // diff = [-2, -4, -6], sum = 4 + 16 + 36 = 56, sqrt(56) ≈ 7.4833
    std::vector<double> a = {-1.0, -2.0, -3.0};
    std::vector<double> b = {1.0, 2.0, 3.0};
    double d = ql::l2_distance(a.data(), b.data(), a.size());
    EXPECT_NEAR(std::sqrt(56.0), d, kEpsilon);
}

TEST(VectorDistanceTest, L2ZeroDimension) {
    double d = ql::l2_distance(nullptr, nullptr, 0);
    EXPECT_NEAR(0.0, d, kEpsilon);
}

TEST(VectorDistanceTest, L2SingleElement) {
    double a[] = {5.0};
    double b[] = {8.0};
    double d = ql::l2_distance(a, b, 1);
    EXPECT_NEAR(3.0, d, kEpsilon);
}

// ── Cosine Distance Tests ──

TEST(VectorDistanceTest, CosineIdenticalVectors) {
    std::vector<double> v = {1.0, 2.0, 3.0};
    double d = ql::cosine_distance(v.data(), v.data(), v.size());
    EXPECT_NEAR(0.0, d, kEpsilon);
}

TEST(VectorDistanceTest, CosineOrthogonalVectors) {
    // a = [1, 0, 0], b = [0, 1, 0] → dot=0, cos=0, distance=1.0
    std::vector<double> a = {1.0, 0.0, 0.0};
    std::vector<double> b = {0.0, 1.0, 0.0};
    double d = ql::cosine_distance(a.data(), b.data(), a.size());
    EXPECT_NEAR(1.0, d, kEpsilon);
}

TEST(VectorDistanceTest, CosineOppositeVectors) {
    // a = [1, 2, 3], b = [-1, -2, -3] → cos=-1, distance=2.0
    std::vector<double> a = {1.0, 2.0, 3.0};
    std::vector<double> b = {-1.0, -2.0, -3.0};
    double d = ql::cosine_distance(a.data(), b.data(), a.size());
    EXPECT_NEAR(2.0, d, kEpsilon);
}

TEST(VectorDistanceTest, CosineZeroNorms) {
    // Both zero vectors → treated as identical → distance 0.0
    std::vector<double> a = {0.0, 0.0, 0.0};
    std::vector<double> b = {0.0, 0.0, 0.0};
    double d = ql::cosine_distance(a.data(), b.data(), a.size());
    EXPECT_NEAR(0.0, d, kEpsilon);
}

TEST(VectorDistanceTest, CosineOneZeroNorm) {
    // One zero vector, one non-zero → dot=0, distance=1.0
    std::vector<double> a = {0.0, 0.0, 0.0};
    std::vector<double> b = {1.0, 2.0, 3.0};
    double d = ql::cosine_distance(a.data(), b.data(), a.size());
    EXPECT_NEAR(1.0, d, kEpsilon);
}

TEST(VectorDistanceTest, CosineZeroDimension) {
    double d = ql::cosine_distance(nullptr, nullptr, 0);
    EXPECT_NEAR(0.0, d, kEpsilon);
}

// ── Inner Product Distance Tests ──

TEST(VectorDistanceTest, InnerProductIdenticalVectors) {
    // a = [1, 2, 3] → dot=1+4+9=14 → distance=-14
    std::vector<double> v = {1.0, 2.0, 3.0};
    double d = ql::inner_product_distance(v.data(), v.data(), v.size());
    EXPECT_NEAR(-14.0, d, kEpsilon);
}

TEST(VectorDistanceTest, InnerProductOrthogonalVectors) {
    // a = [1, 0, 0], b = [0, 1, 0] → dot=0, distance=0
    std::vector<double> a = {1.0, 0.0, 0.0};
    std::vector<double> b = {0.0, 1.0, 0.0};
    double d = ql::inner_product_distance(a.data(), b.data(), a.size());
    EXPECT_NEAR(0.0, d, kEpsilon);
}

TEST(VectorDistanceTest, InnerProductNegativeValues) {
    // a = [1, 2], b = [-3, -4] → dot = -3 + -8 = -11 → distance = 11
    std::vector<double> a = {1.0, 2.0};
    std::vector<double> b = {-3.0, -4.0};
    double d = ql::inner_product_distance(a.data(), b.data(), a.size());
    EXPECT_NEAR(11.0, d, kEpsilon);
}

TEST(VectorDistanceTest, InnerProductZeroDimension) {
    double d = ql::inner_product_distance(nullptr, nullptr, 0);
    EXPECT_NEAR(0.0, d, kEpsilon);
}

// ── Large Dimension Tests ──

TEST(VectorDistanceTest, L2LargeDimension) {
    // Test with 200 elements to exercise SIMD paths.
    constexpr size_t dim = 200;
    std::vector<double> a(dim, 1.0);
    std::vector<double> b(dim, 2.0);
    // diff per element = -1.0, squared = 1.0, sum = 200, sqrt = sqrt(200)
    double d = ql::l2_distance(a.data(), b.data(), dim);
    EXPECT_NEAR(std::sqrt(200.0), d, kEpsilon);
}

TEST(VectorDistanceTest, CosineLargeDimension) {
    constexpr size_t dim = 200;
    std::vector<double> a(dim, 1.0);
    std::vector<double> b(dim, 2.0);
    // dot = 200 * 2 = 400
    // |a| = sqrt(200), |b| = sqrt(800) = sqrt(200*4) = 2*sqrt(200)
    // cos = 400 / (sqrt(200) * 2*sqrt(200)) = 400 / (2*200) = 1.0
    // distance = 0.0
    double d = ql::cosine_distance(a.data(), b.data(), dim);
    EXPECT_NEAR(0.0, d, kEpsilon);
}

TEST(VectorDistanceTest, InnerProductLargeDimension) {
    constexpr size_t dim = 200;
    std::vector<double> a(dim, 1.0);
    std::vector<double> b(dim, 2.0);
    // dot = 200 * 2 = 400, distance = -400
    double d = ql::inner_product_distance(a.data(), b.data(), dim);
    EXPECT_NEAR(-400.0, d, kEpsilon);
}

// ── Consistency Tests ──

TEST(VectorDistanceTest, L2MatchesManualComputation) {
    // Manually compute L2 and compare against function result.
    std::vector<double> a = {1.5, -2.3, 4.7, 0.1, -3.2};
    std::vector<double> b = {2.1, 1.4, -0.5, 3.3, 1.1};

    double manual_sq = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        double diff = a[i] - b[i];
        manual_sq += diff * diff;
    }
    double manual = std::sqrt(manual_sq);

    double result = ql::l2_distance(a.data(), b.data(), a.size());
    EXPECT_NEAR(manual, result, kEpsilon);
}

TEST(VectorDistanceTest, CosineMatchesManualComputation) {
    std::vector<double> a = {1.5, -2.3, 4.7, 0.1, -3.2};
    std::vector<double> b = {2.1, 1.4, -0.5, 3.3, 1.1};

    double dot = 0.0, na2 = 0.0, nb2 = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        na2 += a[i] * a[i];
        nb2 += b[i] * b[i];
    }
    double manual = 1.0 - dot / (std::sqrt(na2) * std::sqrt(nb2));

    double result = ql::cosine_distance(a.data(), b.data(), a.size());
    EXPECT_NEAR(manual, result, kEpsilon);
}

// ── Non-multiple-of-4 dimension tests ──

TEST(VectorDistanceTest, L2OddDimension) {
    // 7 elements — exercises SIMD remainder path when AVX available.
    std::vector<double> a = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0};
    std::vector<double> b = {7.0, 6.0, 5.0, 4.0, 3.0, 2.0, 1.0};
    double d = ql::l2_distance(a.data(), b.data(), a.size());

    double manual_sq = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        double diff = a[i] - b[i];
        manual_sq += diff * diff;
    }
    EXPECT_NEAR(std::sqrt(manual_sq), d, kEpsilon);
}

TEST(VectorDistanceTest, CosineOddDimension) {
    std::vector<double> a = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0};
    std::vector<double> b = {7.0, 6.0, 5.0, 4.0, 3.0, 2.0, 1.0};

    double dot = 0.0, na2 = 0.0, nb2 = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        na2 += a[i] * a[i];
        nb2 += b[i] * b[i];
    }
    double manual = 1.0 - dot / (std::sqrt(na2) * std::sqrt(nb2));

    double result = ql::cosine_distance(a.data(), b.data(), a.size());
    EXPECT_NEAR(manual, result, kEpsilon);
}

// ── Scalar consistency with large vectors ──

TEST(VectorDistanceTest, InnerProductMatchesManualComputation) {
    std::vector<double> a = {1.5, -2.3, 4.7, 0.1, -3.2};
    std::vector<double> b = {2.1, 1.4, -0.5, 3.3, 1.1};

    double dot = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
    }

    double result = ql::inner_product_distance(a.data(), b.data(), a.size());
    EXPECT_NEAR(-dot, result, kEpsilon);
}

}  // namespace unittest
