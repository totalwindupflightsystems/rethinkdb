// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_VECTOR_DISTANCE_HPP_
#define RDB_PROTOCOL_VECTOR_DISTANCE_HPP_

#include <cstddef>

namespace ql {

/* Distance functions for vector similarity search.
 *
 * All functions take pointers to double arrays and a dimension count.
 * The caller is responsible for ensuring that a and b point to valid
 * arrays of at least `dim` elements.
 *
 * SIMD acceleration (AVX2 / AVX-512) is used automatically at runtime
 * when the hardware supports it and the vector is sufficiently large.
 */

/* L2 (Euclidean) distance: sqrt(sum((a[i] - b[i])^2)) */
double l2_distance(const double *a, const double *b, size_t dim);

/* Cosine distance: 1.0 - (dot(a,b) / (|a| * |b|))
 * Returns 0.0 for identical vectors, up to 2.0 for opposite vectors.
 * Returns 0.0 when both vectors have zero norm (considered identical). */
double cosine_distance(const double *a, const double *b, size_t dim);

/* Inner product distance: -dot(a,b)
 * Negated so that "nearest" = smallest distance for max-similarity search. */
double inner_product_distance(const double *a, const double *b, size_t dim);

}  // namespace ql

#endif  // RDB_PROTOCOL_VECTOR_DISTANCE_HPP_
