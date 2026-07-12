# Vector Index Architecture — RethinkDB v2.6

**Status:** Draft (VECTOR-1)  
**Date:** 2026-07-11  
**Author:** Coding Hermes Foreman

## 1. Overview

Add approximate nearest neighbor (ANN) vector search to RethinkDB as a new secondary index type, following the same pattern as multi-indexes, geo-indexes, and FTS-indexes. The initial implementation targets HNSW (Hierarchical Navigable Small World) for state-of-the-art ANN performance with good insert/search trade-offs.

## 2. API Design

### 2.1 Index Creation

```
r.table("docs").indexCreate(
    "embedding_idx",
    r.row("embedding"),
    {vector: {dim: 768, metric: "cosine"}}
)
```

- `vector.dim` — dimensionality (required, 1–32768)
- `vector.metric` — distance metric: `"l2"` (default), `"cosine"`, `"inner_product"`
- `vector.M` — HNSW M parameter (default 16, connections per layer)
- `vector.ef_construction` — HNSW efConstruction (default 200)

### 2.2 Querying

```
// Top-K nearest neighbors
r.table("docs").vectorNear(
    "embedding_idx",
    query_vector,          // r.vector([0.1, 0.2, ...])
    {k: 10}
)

// With filter threshold
r.table("docs").vectorNear(
    "embedding_idx",
    query_vector,
    {k: 10, max_dist: 0.5}
)
```

### 2.3 New ReQL Terms

| Term | Type | Description |
|------|------|-------------|
| `VECTOR` | 200 | Create a vector datum: `r.vector([1.0, 2.0, 3.0])` |
| `VECTOR_NEAR` | 201 | ANN search on vector index |

### 2.4 Vector Datum Type

Vectors are stored as `std::vector<double>` internally. On the wire, they serialize as a packed double array. The proto `DatumType` gets a new `R_VECTOR = 8` variant.

## 3. Data Structures

### 3.1 Following the Sindex Pattern

The vector index follows the exact same extensibility pattern as multi/geo/fts:

**New enum:**
```cpp
enum class sindex_vector_bool_t {
    REGULAR = 0,
    VECTOR  = 1
};
```

**Extended config & disk info:**
```cpp
class sindex_config_t {
    // ... existing fields ...
    sindex_vector_bool_t vector;           // NEW
    size_t vector_dim;                      // NEW (meaningful only if vector==VECTOR)
    std::string vector_metric;              // NEW: "l2", "cosine", "inner_product"
};

struct sindex_disk_info_t {
    // ... existing fields ...
    sindex_vector_bool_t vector;            // NEW
    size_t vector_dim;                      // NEW
    std::string vector_metric;              // NEW
};
```

### 3.2 HNSW Graph Storage

The HNSW graph is NOT stored in the B-tree. It lives in a separate file/block structure:

```
/serializer/
  hnsw_graph.hpp       — HNSW graph header (metadata, level pointers)
  hnsw_graph.cc        — HNSW graph implementation
```

**Graph structure (per index):**
```
HNSWGraph {
    int M;                    // max connections per node per layer
    int M_max0;               // max connections for layer 0 (2*M)
    int ef_construction;      // search width during construction
    size_t dim;               // vector dimensionality
    int ml;                   // current max level
    vector<HNSWNode> nodes;   // node storage
    int entry_point;          // global entry point node id
}

HNSWNode {
    vector<double> vec;       // the embedding
    vector<vector<int>> layers; // per-layer neighbor lists
    store_key_t primary_key;  // reference back to the document
}
```

**Storage format:**
- Vectors + HNSW graph stored in B-tree leaf pages as opaque values
- Each B-tree key = incremental node ID
- Each B-tree value = serialized {vector, primary_key, neighbor_metadata}
- Can be stored inline or as blobs depending on vector size

### 3.3 Alternative: Co-located with B-tree Values

Rather than a separate graph structure, the HNSW graph can be embedded in the existing B-tree:
- B-tree key = store_key_t (the indexed key from the map function, like regular sindexes)
- B-tree value = packed {vector_data, hnsw_neighbor_list}
- HNSW layer structure stored in a separate "header" block (like sindex superblock)
- This reuses all existing B-tree concurrency, caching, and serialization

**RECOMMENDED APPROACH:** Co-locate with B-tree. It's simpler, reuses the proven storage engine, and avoids building a new concurrent data structure.

## 4. Algorithm Choice: HNSW

### 4.1 Why HNSW

- **State-of-the-art:** Best recall/speed trade-off in academic benchmarks
- **Incremental:** Supports online insertions (no retraining like IVFFlat)
- **Proven:** Used by pgvector, Elasticsearch, Weaviate, Milvus, etc.
- **Self-contained:** Pure C++ implementation, no external deps

### 4.2 Core Operations

**INSERT:** 
1. Sample level l from exponential distribution (controlled by m_L)
2. Find entry point from top layer down to layer l using greedy search
3. For each layer from min(l, ml) down to 0:
   - Find ef_construction nearest neighbors
   - Select M (or M_max0) best connections
   - Update neighbor lists

**SEARCH (k-NN):**
1. Start at entry point, traverse from top layer down to layer 0
2. At each layer, greedily move to nearest neighbor
3. At layer 0, perform beam search with width ef_search
4. Return top-k results

**DELETE:**
- Mark node as deleted (tombstone)
- Periodically compact to remove tombstones

### 4.3 Distance Functions

```cpp
// L2 (Euclidean)
double l2_distance(const double* a, const double* b, size_t dim);

// Cosine similarity → distance (1 - cos_sim)
double cosine_distance(const double* a, const double* b, size_t dim);

// Inner product → distance (negated for max-similarity search)
double inner_product_distance(const double* a, const double* b, size_t dim);
```

SIMD acceleration (AVX2/AVX-512) for vectors ≥ 128 dimensions.

## 5. Integration Points

### 5.1 Storage Layer (`src/btree/`, `src/serializer/`)

- Reuse `btree/secondary_operations.*` for sindex block management
- New file: `src/btree/hnsw_ops.hpp` / `.cc` — HNSW-specific B-tree value operations
- Serializer: no changes needed; HNSW data is opaque to the serializer

### 5.2 Query Layer (`src/rdb_protocol/`)

- `ql2.proto`: Add VECTOR=200, VECTOR_NEAR=201 to TermType
- `terms/vector_term.hpp/.cc`: Implement vector creation term
- `terms/vector_near_term.hpp/.cc`: Implement ANN search term
- `datum.hpp/.cc`: Add R_VECTOR type, vector construction/access methods
- `context.hpp`: Extend `sindex_config_t` with vector fields
- `btree.hpp`: Extend `sindex_disk_info_t` with vector fields
- `store.hpp/.cc`: Wire vector index creation, query dispatch
- `sindex_create_term.hpp/.cc`: Add vector optarg parsing

### 5.3 Serialization (`src/containers/archive/`)

- Serialize/deserialize vector fields in sindex_config_t and sindex_disk_info_t
- Backward compatible: new fields default to REGULAR (non-vector)
- Add serialization for vector data in B-tree values

### 5.4 Build System

- Add new .cc files to `src/Makefile` or the build configuration
- No new external dependencies (HNSW is self-contained C++)

## 6. Implementation Plan

### Phase 1: Foundation (VECTOR-2, VECTOR-3)
- Vector datum type (R_VECTOR)
- Distance functions with tests
- Wire protocol updates

### Phase 2: Storage (VECTOR-6, VECTOR-7)
- Sindex config/disk_info extensions
- Serialization
- Index creation optarg parsing

### Phase 3: HNSW (VECTOR-4)
- HNSW algorithm implementation
- Insert, search, delete
- B-tree integration

### Phase 4: Query (VECTOR-6 continued)
- VECTOR_NEAR ReQL term
- Query dispatch and execution
- Result ordering by distance

### Phase 5: Polish (VECTOR-8)
- Comprehensive tests
- SIMD optimizations
- Documentation

## 7. Open Questions

1. **Parallel build:** Should the HNSW graph be built in parallel during post-construction? The existing sindex post-construction framework supports this.
2. **ef_search parameter:** Should ef_search be a query-time parameter (like pgvector) or fixed at index creation?
3. **Memory vs disk:** HNSW graphs are memory-hungry. Should we use memory-mapped files or explicit page cache?
4. **IVFFlat as option:** Should IVFFlat be a separate index type or a parameter on the vector index?
5. **Dimension validation:** Should we enforce consistent dimensions across all vectors in an index?

## 8. References

- Malkov & Yashunin (2018): "Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs"
- pgvector source: https://github.com/pgvector/pgvector
- RethinkDB sindex architecture: `src/btree/secondary_operations.*`, `src/rdb_protocol/btree.hpp`
- Previous FTS implementation: FTS-1 through FTS-4 tasks (2026-07-09 to 2026-07-11)
