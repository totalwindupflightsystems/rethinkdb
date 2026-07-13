// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_IVFFLAT_HPP_
#define RDB_PROTOCOL_IVFFLAT_HPP_

#include <cstddef>
#include <utility>
#include <vector>

#include "btree/keys.hpp"
#include "containers/archive/archive.hpp"
#include "errors.hpp"

namespace ql {

/* `ivfflat_index_t` implements an Inverted File with Flat compression (IVFFlat)
 * for approximate nearest neighbor search on vector data.
 *
 * IVFFlat (Jégou et al., 2011) is a two-phase approach:
 *   1. TRAINING: Run k-means clustering on a representative sample of vectors
 *      to produce `nlist` centroids.
 *   2. INDEXING: Each vector is assigned to its nearest centroid and stored
 *      in that centroid's inverted list (flat, no compression).
 *   3. SEARCH: The query is compared to centroids; the `nprobe` nearest
 *      centroids are selected, and their full inverted lists are scanned
 *      for candidates. Top-k results are returned.
 *
 * Compared to HNSW, IVFFlat offers faster indexing and lower memory but
 * requires a training phase and has lower recall at the same speed.
 *
 * Configuration:
 *   - nlist:    number of clusters / centroids (default 100)
 *   - nprobe:   number of centroids to probe during search (default 10)
 *   - dim:      vector dimensionality
 *
 * Supported distance metrics:
 *   - L2 (Euclidean)
 *   - COSINE (1 - cosine similarity)
 *   - INNER_PRODUCT (-dot product, for max-similarity search)
 */
class ivfflat_index_t {
public:
    enum class metric_t { L2, COSINE, INNER_PRODUCT };

    /* Construct an empty IVFFlat index.
     * nlist is the number of clusters (centroids).
     * nprobe is the default number of centroids to probe during search. */
    ivfflat_index_t(int nlist = 100, int nprobe = 10, size_t dim = 0,
                    metric_t metric = metric_t::L2);

    /* Train the index by computing centroids from a set of training vectors.
     * The vectors are used to cluster into `nlist` groups via k-means.
     * This must be called before any insert/search operations.
     * Can be called multiple times to retrain. */
    void train(const std::vector<std::vector<double>> &training_vectors,
               int max_iterations = 20);

    /* Insert a vector with its primary key.
     * The vector must have exactly `dim` elements.
     * Training must have been completed first. */
    void insert(const double *vec, const store_key_t &key);

    /* Search for the k nearest neighbors of a query vector.
     * nprobe controls how many centroids to probe; higher values are
     * more accurate but slower. Returns (distance, primary_key) pairs
     * sorted by distance. */
    std::vector<std::pair<double, store_key_t>> search_knn(
        const double *query, int k, int nprobe = -1) const;

    /* Remove a vector by its primary key (tombstone).
     * Note: linear scan through inverted lists — O(N). */
    void remove(const store_key_t &key);

    /* Number of vectors in the index (excluding deleted ones). */
    size_t size() const;

    /* Whether the index has been trained. */
    bool is_trained() const { return trained_; }

    /* Configuration accessors. */
    int get_nlist() const { return nlist_; }
    int get_nprobe() const { return nprobe_; }
    size_t get_dim() const { return dim_; }
    metric_t get_metric() const { return metric_; }
    const std::vector<std::vector<double>> &get_centroids() const {
        return centroids_;
    }

    /* Serialization support. */
    RDB_DECLARE_ME_SERIALIZABLE(ivfflat_index_t);

private:
    /* Inverted list for a single cluster. */
    struct inverted_list_t {
        std::vector<std::vector<double>> vectors;
        std::vector<store_key_t> keys;
        std::vector<bool> deleted;
    };

    int nlist_;
    int nprobe_;
    size_t dim_;
    metric_t metric_;
    bool trained_;

    /* Centroids: centroids_[i] = centroid vector for cluster i. */
    std::vector<std::vector<double>> centroids_;

    /* Inverted lists: one per cluster. */
    std::vector<inverted_list_t> lists_;

    /* Total vector count (non-deleted). */
    size_t total_count_;

    DISABLE_COPYING(ivfflat_index_t);

    /* Compute distance between two vectors using the configured metric. */
    double compute_distance(const double *a, const double *b) const;

    /* Find the nearest centroid to a query vector.
     * Returns (centroid_index, distance). */
    std::pair<int, double> find_nearest_centroid(const double *query) const;

    /* Find the nprobe nearest centroids.
     * Returns (centroid_index, distance) pairs sorted by distance. */
    std::vector<std::pair<int, double>> find_nearest_centroids(
        const double *query, int nprobe) const;

    /* K-means clustering on training vectors.
     * Returns the final centroids. */
    std::vector<std::vector<double>> kmeans(
        const std::vector<std::vector<double>> &data,
        int k, int max_iterations) const;
};

}  // namespace ql

#endif  // RDB_PROTOCOL_IVFFLAT_HPP_
