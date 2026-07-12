// Copyright 2026 RethinkDB, all rights reserved.
#include "rdb_protocol/vector_distance.hpp"

#include <cmath>
#include <cstddef>

#ifdef __AVX2__
#include <immintrin.h>
#endif
#ifdef __AVX512F__
#include <immintrin.h>
#endif

namespace ql {

// ── Portable scalar implementations ──

namespace {

double l2_distance_scalar(const double *a, const double *b, size_t dim) {
    double sum = 0.0;
    for (size_t i = 0; i < dim; ++i) {
        double diff = a[i] - b[i];
        sum += diff * diff;
    }
    return std::sqrt(sum);
}

double cosine_distance_scalar(const double *a, const double *b, size_t dim) {
    double dot = 0.0;
    double norm_a_sq = 0.0;
    double norm_b_sq = 0.0;
    for (size_t i = 0; i < dim; ++i) {
        dot += a[i] * b[i];
        norm_a_sq += a[i] * a[i];
        norm_b_sq += b[i] * b[i];
    }
    // Guard against zero-norm vectors: both zero → distance 0 (identical).
    if (norm_a_sq == 0.0 && norm_b_sq == 0.0) {
        return 0.0;
    }
    // One zero-norm vector, one non-zero: dot is 0, cos_sim is 0 → distance 1.0
    if (norm_a_sq == 0.0 || norm_b_sq == 0.0) {
        return 1.0;
    }
    double norm_product = std::sqrt(norm_a_sq) * std::sqrt(norm_b_sq);
    // Clamp cosine similarity to [-1, 1] to avoid tiny floating-point excursions.
    double cos_sim = dot / norm_product;
    if (cos_sim > 1.0) cos_sim = 1.0;
    if (cos_sim < -1.0) cos_sim = -1.0;
    return 1.0 - cos_sim;
}

double inner_product_distance_scalar(const double *a, const double *b, size_t dim) {
    double dot = 0.0;
    for (size_t i = 0; i < dim; ++i) {
        dot += a[i] * b[i];
    }
    return -dot;
}

}  // anonymous namespace

// ── AVX2 implementations (4 doubles per 256-bit register) ──

#ifdef __AVX2__

namespace {

/* Horizontal sum of four doubles in an __m256d register. */
inline double hsum_pd_avx2(__m256d v) {
    // v = [d0, d1, d2, d3]
    // Permute: swap high and low 128-bit lanes, producing [d2, d3, d0, d1]
    __m256d v_perm = _mm256_permute2f128_pd(v, v, 0x01);
    // Add: [d0+d2, d1+d3, d2+d0, d3+d1]
    v = _mm256_add_pd(v, v_perm);
    // Horizontal add within each 128-bit lane: [d0+d2+d1+d3, d0+d2+d1+d3, d2+d0+d3+d1, d2+d0+d3+d1]
    v = _mm256_hadd_pd(v, v);
    return _mm_cvtsd_f64(_mm256_castpd256_pd128(v));
}

double l2_distance_avx2(const double *a, const double *b, size_t dim) {
    __m256d accum = _mm256_setzero_pd();
    size_t i = 0;

    // Process 4 elements at a time.
    size_t simd_end = (dim / 4) * 4;
    for (; i < simd_end; i += 4) {
        __m256d va = _mm256_loadu_pd(a + i);
        __m256d vb = _mm256_loadu_pd(b + i);
        __m256d diff = _mm256_sub_pd(va, vb);
        accum = _mm256_add_pd(accum, _mm256_mul_pd(diff, diff));
    }

    double sum = hsum_pd_avx2(accum);

    // Scalar tail for remaining elements.
    for (; i < dim; ++i) {
        double diff = a[i] - b[i];
        sum += diff * diff;
    }

    return std::sqrt(sum);
}

double cosine_distance_avx2(const double *a, const double *b, size_t dim) {
    __m256d accum_dot = _mm256_setzero_pd();
    __m256d accum_norm_a = _mm256_setzero_pd();
    __m256d accum_norm_b = _mm256_setzero_pd();
    size_t i = 0;

    size_t simd_end = (dim / 4) * 4;
    for (; i < simd_end; i += 4) {
        __m256d va = _mm256_loadu_pd(a + i);
        __m256d vb = _mm256_loadu_pd(b + i);
        accum_dot = _mm256_add_pd(accum_dot, _mm256_mul_pd(va, vb));
        accum_norm_a = _mm256_add_pd(accum_norm_a, _mm256_mul_pd(va, va));
        accum_norm_b = _mm256_add_pd(accum_norm_b, _mm256_mul_pd(vb, vb));
    }

    double dot = hsum_pd_avx2(accum_dot);
    double norm_a_sq = hsum_pd_avx2(accum_norm_a);
    double norm_b_sq = hsum_pd_avx2(accum_norm_b);

    for (; i < dim; ++i) {
        dot += a[i] * b[i];
        norm_a_sq += a[i] * a[i];
        norm_b_sq += b[i] * b[i];
    }

    if (norm_a_sq == 0.0 && norm_b_sq == 0.0) {
        return 0.0;
    }
    if (norm_a_sq == 0.0 || norm_b_sq == 0.0) {
        return 1.0;
    }
    double norm_product = std::sqrt(norm_a_sq) * std::sqrt(norm_b_sq);
    double cos_sim = dot / norm_product;
    if (cos_sim > 1.0) cos_sim = 1.0;
    if (cos_sim < -1.0) cos_sim = -1.0;
    return 1.0 - cos_sim;
}

double inner_product_distance_avx2(const double *a, const double *b, size_t dim) {
    __m256d accum = _mm256_setzero_pd();
    size_t i = 0;

    size_t simd_end = (dim / 4) * 4;
    for (; i < simd_end; i += 4) {
        __m256d va = _mm256_loadu_pd(a + i);
        __m256d vb = _mm256_loadu_pd(b + i);
        accum = _mm256_add_pd(accum, _mm256_mul_pd(va, vb));
    }

    double dot = hsum_pd_avx2(accum);

    for (; i < dim; ++i) {
        dot += a[i] * b[i];
    }

    return -dot;
}

}  // anonymous namespace

#endif  // __AVX2__

// ── AVX-512 implementations (8 doubles per 512-bit register) ──

#ifdef __AVX512F__

namespace {

/* Horizontal sum of eight doubles in an __m512d register. */
inline double hsum_pd_avx512(__m512d v) {
    // Reduce across the 512-bit vector.
    return _mm512_reduce_add_pd(v);
}

double l2_distance_avx512(const double *a, const double *b, size_t dim) {
    __m512d accum = _mm512_setzero_pd();
    size_t i = 0;

    size_t simd_end = (dim / 8) * 8;
    for (; i < simd_end; i += 8) {
        __m512d va = _mm512_loadu_pd(a + i);
        __m512d vb = _mm512_loadu_pd(b + i);
        __m512d diff = _mm512_sub_pd(va, vb);
        accum = _mm512_add_pd(accum, _mm512_mul_pd(diff, diff));
    }

    double sum = hsum_pd_avx512(accum);

    // Scalar tail.
    for (; i < dim; ++i) {
        double diff = a[i] - b[i];
        sum += diff * diff;
    }

    return std::sqrt(sum);
}

double cosine_distance_avx512(const double *a, const double *b, size_t dim) {
    __m512d accum_dot = _mm512_setzero_pd();
    __m512d accum_norm_a = _mm512_setzero_pd();
    __m512d accum_norm_b = _mm512_setzero_pd();
    size_t i = 0;

    size_t simd_end = (dim / 8) * 8;
    for (; i < simd_end; i += 8) {
        __m512d va = _mm512_loadu_pd(a + i);
        __m512d vb = _mm512_loadu_pd(b + i);
        accum_dot = _mm512_add_pd(accum_dot, _mm512_mul_pd(va, vb));
        accum_norm_a = _mm512_add_pd(accum_norm_a, _mm512_mul_pd(va, va));
        accum_norm_b = _mm512_add_pd(accum_norm_b, _mm512_mul_pd(vb, vb));
    }

    double dot = hsum_pd_avx512(accum_dot);
    double norm_a_sq = hsum_pd_avx512(accum_norm_a);
    double norm_b_sq = hsum_pd_avx512(accum_norm_b);

    for (; i < dim; ++i) {
        dot += a[i] * b[i];
        norm_a_sq += a[i] * a[i];
        norm_b_sq += b[i] * b[i];
    }

    if (norm_a_sq == 0.0 && norm_b_sq == 0.0) {
        return 0.0;
    }
    if (norm_a_sq == 0.0 || norm_b_sq == 0.0) {
        return 1.0;
    }
    double norm_product = std::sqrt(norm_a_sq) * std::sqrt(norm_b_sq);
    double cos_sim = dot / norm_product;
    if (cos_sim > 1.0) cos_sim = 1.0;
    if (cos_sim < -1.0) cos_sim = -1.0;
    return 1.0 - cos_sim;
}

double inner_product_distance_avx512(const double *a, const double *b, size_t dim) {
    __m512d accum = _mm512_setzero_pd();
    size_t i = 0;

    size_t simd_end = (dim / 8) * 8;
    for (; i < simd_end; i += 8) {
        __m512d va = _mm512_loadu_pd(a + i);
        __m512d vb = _mm512_loadu_pd(b + i);
        accum = _mm512_add_pd(accum, _mm512_mul_pd(va, vb));
    }

    double dot = hsum_pd_avx512(accum);

    for (; i < dim; ++i) {
        dot += a[i] * b[i];
    }

    return -dot;
}

}  // anonymous namespace

#endif  // __AVX512F__

// ── Public dispatch functions ──

double l2_distance(const double *a, const double *b, size_t dim) {
    if (dim == 0) {
        return 0.0;
    }
#ifdef __AVX512F__
    if (dim >= 256) {
        return l2_distance_avx512(a, b, dim);
    }
#endif
#ifdef __AVX2__
    if (dim >= 128) {
        return l2_distance_avx2(a, b, dim);
    }
#endif
    return l2_distance_scalar(a, b, dim);
}

double cosine_distance(const double *a, const double *b, size_t dim) {
    if (dim == 0) {
        return 0.0;
    }
#ifdef __AVX512F__
    if (dim >= 256) {
        return cosine_distance_avx512(a, b, dim);
    }
#endif
#ifdef __AVX2__
    if (dim >= 128) {
        return cosine_distance_avx2(a, b, dim);
    }
#endif
    return cosine_distance_scalar(a, b, dim);
}

double inner_product_distance(const double *a, const double *b, size_t dim) {
    if (dim == 0) {
        return 0.0;
    }
#ifdef __AVX512F__
    if (dim >= 256) {
        return inner_product_distance_avx512(a, b, dim);
    }
#endif
#ifdef __AVX2__
    if (dim >= 128) {
        return inner_product_distance_avx2(a, b, dim);
    }
#endif
    return inner_product_distance_scalar(a, b, dim);
}

}  // namespace ql
