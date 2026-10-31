# Track D: Traverse

## Approach
Our solution is written in pure C (C11/POSIX) with 0 external dependencies:
1. **Compressed Sparse Row (CSR) Layout:** Edges are ingested via zero-copy `mmap`. Node degrees are determined in Pass 1, and edges are populated directly in Pass 2 into a flat contiguous array, using only ~40 MB of RAM.
2. **4-ary Min-Heap:** Replaces traditional binary heaps with a 4-ary layout. Each parent node fits its 4 children in a single 64-byte cache line, cutting branch depth in half and speeding up relaxation.
3. **Epoch-Based Visited Array:** Each thread maintains a persistent distance array and a generation counter (`epoch`). Nodes are marked active for the current query without clearing the entire array, achieving $O(1)$ query reset.
4. **Lock-Free Multi-Threading:** 2,000 queries are distributed dynamically across CPU cores using an atomic counter (`stdatomic.h`) to guarantee optimal load balancing.

## Dependencies
None (C Standard Library and POSIX only).

## With another 24 hours
Implement Bidirectional Dijkstra with A* landmark heuristics (ALT algorithm) to reduce search frontiers on long-distance queries.