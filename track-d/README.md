# Track D: Traverse

## Approach

Pure C11/POSIX, no dependencies. Every query is an exact Dijkstra that stops the
moment the target is settled, but the usual constant factors are attacked from
five sides: the node order, the heap, the per-node state, the graph build, and
the queries that never need a search at all.

1. **Radix heap instead of a comparison heap.** All weights are non-negative, so
   Dijkstra pops keys in non-decreasing order. That makes the key of the last pop
   a valid lower bound, and the bucket of a new key is simply the highest bit
   where it differs from it (`64 - clz(key ^ last)`, 65 buckets). Push and pop are
   amortised O(1): no comparisons, no sift, no tree. Measured on this machine,
   one full-scale dataset went from 29 s of single-core Dijkstra with the old
   4-ary heap to 6.8 s.

2. **Relabelling the graph in BFS order.** This is the largest single win.
   A Dijkstra search only ever touches the ball of nodes around `s`, but with the
   input numbering that ball is scattered over the whole 1.74 M-node array: a
   median query settles 512 nodes that live in 512 different cache lines of
   `tag[]`, `head[]` and `edges[]`. Renumbering the nodes in breadth-first order
   from a high-out-degree start node turns a median ball into a band of about
   2 K consecutive ids and the mean ball (12.3 K settled nodes) into ~48 K ids,
   so the distance table, the offsets and the edge blocks of a query stay in
   cache. The CSR is rewritten in the new order and the queries are mapped
   through the same permutation, so the answers are untouched: this is only a
   change of names. One thread: 200 full-size queries go from 0.36 s to 0.16 s;
   on the whole run the query phase drops by 1.9x.

3. **One 64-bit word per node for visited-and-distance.** A shortest path has at
   most `n - 1` edges of weight below 2^32, so every distance and every candidate
   is below `n * 2^32`. The bits above that are free, so they hold a per-query
   generation stamp and each node stores `(stamp << shift) | distance`
   (`shift` is derived from `n` at startup). "Seen in this query?" and "current
   distance" become a single load from a single cache line, where the obvious
   `visited[]` + `dist[]` pair costs two; the relaxation loop is memory-latency
   bound, so this is the cheapest large win available. No clearing between
   queries: the stamp rolls over and the table is zeroed once every
   `2^shift` queries per worker.

4. **Same word, exact arithmetic.** The split is not a heuristic: candidates are
   provably below `n * 2^32` and the shift keeps two extra bits, so the encoding
   is exact for every answer including the ones above 2^32-1. Verified by the
   SHA-256 gate and by differential fuzzing against the Python reference on 420
   random graphs (zero weights, `2^32-1` weights, islands, isolated nodes).

5. **Parallel graph build, one parse.** The 92 MB of text are read exactly once:
   each thread owns a line-aligned byte range, parses the three integers, counts
   the tail into a shared degree array and drops `(u, v, w)` into its own slice
   of an edge buffer (12 bytes per row). The degree array is prefix-summed into
   the CSR offsets and the text is unmapped immediately, so it never competes
   with the edge buffer for RSS. Pass 2 then fills the CSR **by node range**: a
   thread rescans the parsed buffer and keeps only the rows whose tail falls in
   its slice of the ids. Every write of a thread therefore stays inside a
   contiguous, cache-resident slice of the edge array, no cursor needs an atomic
   and no edge is touched by two threads. That fixed the two problems of the old
   scatter — the random 8-byte atomic into a 40 MB array, and the second parse of
   the text. The BFS that decides the new numbering is serial, because the order
   *is* the product; everything that only rewrites data is threaded.

6. **Exact O(1) shortcuts.** `s == t` is 0; a source with no out-edges and a
   target with no in-edges are `-1` without touching the graph. In-degrees are
   needed only for query targets, so a bitmap over the (at most a few thousand)
   targets is built during the scatter instead of bumping an `n`-entry array once
   per edge. This is what removed the two whole-graph scans that used to be the
   wall-clock stragglers.

7. **Prefetch and dispatch.** The relaxation loop prefetches `tag[]` four edges
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
so the idea was dropped rather than kept for looks. With the relabelled graph it
would also need a transposed CSR, which is another 40 MB and another scatter.

A **parallel** BFS for the numbering was tried twice. The first version used a
pthread barrier per BFS level; a barrier costs 105 us on this box and the graph
is 2 600 levels deep, so that is 0.27 s of pure synchronisation. The second was
barrier-free: threads claim chunks of a shared work bag, an atomic bitmap marks
discoveries, and a thread only counts as active once it has claimed work. That
runs in 15 ms, but each thread dives through its own local queue before handing
work back, so ids are handed out along deep excursions instead of level by level.
Sorting the nodes by level afterwards does not repair it, because the order
*inside* a level is spatially scrambled: the measured query time went back to
exactly that of the unrelabelled graph. The serial BFS costs ~0.1 s and buys
1.9x on 0.7 s of queries, so it stays.

A 4-ary comparison heap was measured against the radix heap on cache-resident
data: 0.43 s vs 0.22 s for the same queries. Not close.

## Dependencies

None (C Standard Library and POSIX only).

## With another 24 hours

A real route-planning index: contraction hierarchies or ALT landmarks. Both need
preprocessing that is charged to the run. ALT is out on the numbers: one full
landmark Dijkstra costs about a quarter of the whole query phase, and the edges
of this graph are cheap local streets next to 4x faster highways, so a geometric
bound alone is weak. A contraction hierarchy would pay for itself, but building
one in a run of this size does not.

The measured split is now `build 0.19 s + numbering 0.17 s + permute 0.04 s +
queries 0.45 s`, so the two remaining items are:

- **Fewer settled nodes.** Batching several heap pops and relaxing their edges at
  the same time would raise memory-level parallelism: one core scales 2.2x to
  eight on this box, which is the signature of a latency-bound dependent load
  chain (pop -> `head[u]` -> `edges[]`) rather than of bandwidth or arithmetic.
- **A cheaper numbering.** The BFS is serial, ~0.1 s; it is 12 % of the run and
  the only part that does not use the machine. A parallel BFS that keeps the
  order inside a level would be worth 10 % on its own.

