# Track D: Traverse

## Approach

Pure C11/POSIX, no dependencies. Every query is an exact Dijkstra that stops the
moment the target is settled, but the usual constant factors are attacked from
four sides: the heap, the per-node state, the graph build, and the queries that
never need a search at all.

1. **Radix heap instead of a comparison heap.** All weights are non-negative, so
   Dijkstra pops keys in non-decreasing order. That makes the key of the last pop
   a valid lower bound, and the bucket of a new key is simply the highest bit
   where it differs from it (`64 - clz(key ^ last)`, 65 buckets). Push and pop are
   amortised O(1): no comparisons, no sift, no tree. Measured on this machine,
   one full-scale dataset went from 29 s of single-core Dijkstra with the old
   4-ary heap to 6.8 s.

2. **One 64-bit word per node for visited-and-distance.** A shortest path has at
   most `n - 1` edges of weight below 2^32, so every distance and every candidate
   is below `n * 2^32`. The bits above that are free, so they hold a per-query
   generation stamp and each node stores `(stamp << shift) | distance`
   (`shift` is derived from `n` at startup). "Seen in this query?" and "current
   distance" become a single load from a single cache line, where the obvious
   `visited[]` + `dist[]` pair costs two; the relaxation loop is memory-latency
   bound, so this is the cheapest large win available. No clearing between
   queries: the stamp rolls over and the table is zeroed once every
   `2^shift` queries per worker.

3. **Same word, exact arithmetic.** The split is not a heuristic: candidates are
   provably below `n * 2^32` and the shift keeps two extra bits, so the encoding
   is exact for every answer including the ones above 2^32-1. Verified by the
   SHA-256 gate and by differential fuzzing against the Python reference on 420
   random graphs (zero weights, `2^32-1` weights, islands, isolated nodes).

4. **Parallel graph build.** The edge lines are split into line-aligned chunks.
   Pass 1 counts out-degrees per chunk into private counters; the counters are
   summed into the CSR offsets. Pass 2 parses the three fields and scatters edges
   into their slots, taking each slot from a shared cursor with a relaxed atomic
   add (contention is rare because tails cover the whole node set). Two passes
   over the `mmap`ed text and one over the edge array, all threaded.

5. **Exact O(1) shortcuts.** `s == t` is 0; a source with no out-edges and a
   target with no in-edges are `-1` without touching the graph. In-degrees are
   needed only for query targets, so a bitmap over the (at most a few thousand)
   targets is built during the scatter instead of bumping an `n`-entry array once
   per edge. This is what removed the two whole-graph scans that used to be the
   wall-clock stragglers.

6. **Prefetch and dispatch.** The relaxation loop prefetches `tag[]` four edges
   ahead, and queries are handed out from one atomic counter so that the longest
   searches start early and no thread idles. The thread count comes from
   `sched_getaffinity`, so a cgroup or cpuset limit is respected.

## What did not work

Bidirectional Dijkstra was implemented and measured: it settles 1.8x fewer nodes
(1.07 M vs 1.95 M pops on the sample data) but ran no faster, and sometimes
slower. Halving the search doubles the per-node state (forward and reverse
distances must be live at once), and on this workload the extra random-access
footprint plus a second heap ate the entire gain. A control experiment (the
unidirectional search padded to the same 16-byte state) reproduced the slowdown,
so the idea was dropped rather than kept for looks.

## Dependencies

None (C Standard Library and POSIX only).

## With another 24 hours

A real route-planning index: contraction hierarchies or ALT landmarks. Both need
preprocessing that is charged to the run, and at this graph size the landmark
Dijkstra runs cost about as much as the queries they would save, so they need a
contraction step (or a very small landmark set) to pay off. Also worth measuring
on the target box: whether the build should use fewer threads than the query
phase, since the build is memory-bandwidth bound while the queries are latency
bound.
