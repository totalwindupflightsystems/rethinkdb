// Copyright 2026 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_HNSW_HPP_
#define RDB_PROTOCOL_HNSW_HPP_

#include <cstddef>
#include <utility>
#include <vector>

#include "btree/keys.hpp"
#include "containers/archive/archive.hpp"
#include "errors.hpp"

namespace ql {

/* `hnsw_graph_t` implements a Hierarchical Navigable Small World (HNSW) graph
 * for approximate nearest neighbor search on vector data.
 *
 * The HNSW algorithm (Malkov & Yashunin, 2016) builds a multi-layer graph where
 * each layer is a proximity graph. Search starts at the top (sparsest) layer
 * and descends, using the lower (denser) layers for refinement.
 *
 * Configuration:
 *   - M:               max connections per node per layer (default 16)
 *   - M_max0:          max connections for layer 0 = 2 * M
 *   - ef_construction: search width during construction (default 200)
 *   - dim:             vector dimensionality
 *
 * Supported distance metrics:
 *   - L2 (Euclidean)
 *   - COSINE (1 - cosine similarity)
 *   - INNER_PRODUCT (-dot product, for max-similarity search)
 */
class hnsw_graph_t {
public:
    enum class metric_t { L2, COSINE, INNER_PRODUCT };

    /* Construct an empty HNSW graph.
     * M is the max connections per node per layer (layer 0 uses 2*M).
     * ef_construction controls search thoroughness during insertion. */
    hnsw_graph_t(int M = 16, int ef_construction = 200, size_t dim = 0,
                 metric_t metric = metric_t::L2);

    /* Insert a vector with its primary key.
     * The vector must have exactly `dim` elements. */
    void insert(const double *vec, const store_key_t &key);

    /* Search for the k nearest neighbors of a query vector.
     * ef_search controls search width; higher values are more accurate
     * but slower. Returns (distance, primary_key) pairs sorted by distance. */
    std::vector<std::pair<double, store_key_t>> search_knn(
        const double *query, int k, int ef_search = 100) const;

    /* Mark a node as deleted by its primary key.
     * Deleted nodes are skipped during search but remain in the graph
     * structure (tombstone). */
    void remove(const store_key_t &key);

    /* Number of nodes (including deleted ones). */
    size_t size() const { return nodes_.size(); }
    bool empty() const { return nodes_.empty(); }

    /* Configuration accessors. */
    int get_M() const { return M_; }
    int get_M_max0() const { return M_max0_; }
    int get_ef_construction() const { return ef_construction_; }
    size_t get_dim() const { return dim_; }
    metric_t get_metric() const { return metric_; }

    /* Serialization support. */
    RDB_DECLARE_ME_SERIALIZABLE(hnsw_graph_t);

private:
    /* Internal node representation. */
    struct node_t {
        std::vector<double> vec;
        // per-layer neighbor lists: layers_[layer][i] = node index
        std::vector<std::vector<int>> layers_;
        store_key_t primary_key;
        bool deleted = false;
    };

    int M_;
    int M_max0_;
    int ef_construction_;
    size_t dim_;
    int ml_;             // current maximum level in the graph
    metric_t metric_;
    std::vector<node_t> nodes_;
    int entry_point_;    // index of the graph entry point, -1 if empty

    DISABLE_COPYING(hnsw_graph_t);

    /* Sample a random level from an exponential distribution.
     * m_L = 1 / log(M) normalizes the distribution. */
    int sample_level() const;

    /* Greedy single-path search within one layer.
     * Starts from the given entry point and follows the nearest neighbor
     * path until a local minimum is reached. Returns the node index. */
    int greedy_search_layer(
        const double *query, int ep, int layer) const;

    /* Multi-candidate search within one layer using a beam of `ef` candidates.
     * Returns (distance, node_id) pairs sorted by distance. */
    std::vector<std::pair<double, int>> search_layer(
        const double *query,
        const std::vector<int> &entry_points,
        int layer, int ef) const;

    /* Select up to M_max nearest neighbors from candidates.
     * Simple heuristic: return the M_max closest by distance.
     * Full RNG pruning is not implemented in v1. */
    std::vector<int> select_neighbors(
        const double *query,
        const std::vector<std::pair<double, int>> &candidates,
        int M_max) const;

    /* Compute distance between two vectors using the configured metric. */
    double compute_distance(const double *a, const double *b) const;
};

}  // namespace ql

#endif  // RDB_PROTOCOL_HNSW_HPP_
