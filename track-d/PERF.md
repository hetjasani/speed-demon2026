# Track D: performance log

Every step from the reference to the final version. Board run IDs are recorded
when a step is submitted; the numbers below are measured locally, so the run ID
column is `local` until a live-board run replaces it.

| Step | Change | Run ID | T (s) | M (MB) |
|---|---|---|---|---|
| 0 | The reference, unchanged (spec, M4 Pro) | - | 43.0 - 46.8 | 915 |
| 1 | Original pure-C version: CSR + 4-ary heap + 16 threads | local | 5.64 | 301 |
| 2 | Radix heap, packed epoch+distance word, parallel two-pass CSR, exact shortcuts, prefetch | local | 2.03 | 200 |
| 3 | BFS-order relabelling of the graph, shared degree counters, 16 build threads | local | 1.13 | 177 |

Steps 1 and 2 are the same binary pair, alternating run 1 / run 2 on one machine
(5 rounds, `min` of each; medians 6.48 s and 2.49 s). On `sample`: 0.18 s and
0.09 s. Peak RSS is from `/usr/bin/time -v`-style `%M`.

Step 3 was measured against step 2 interleaved on `full`, two rounds:

| Round | Version | Wall | User | Query phase | Peak RSS |
|---|---|---|---|---|---|
| 1 | step 2 | 1.61 s | 10.74 s | 1.25 s | 200 MB |
| 1 | step 3 | 1.13 s | 6.80 s | 0.67 s | 177 MB |
| 2 | step 2 | 1.75 s | 12.44 s | 1.41 s | 200 MB |
| 2 | step 3 | 1.43 s | 8.28 s | 0.82 s | 177 MB |

Both versions write the same `full` hash. Per phase on step 3 (`full`):
`pass1=0.07-0.08, prefix=0.01, pass2=0.19-0.23, bfs+deg=0.14-0.22,
perm=0.05-0.06, query=0.67-0.82`, i.e. preprocessing is now 40 % of the run and
the query phase is 1.9x cheaper than in step 2. Query-only, 200 queries on the
full graph, `min` of 4 interleaved runs: step 2 `0.144-0.187 s`, step 3
`0.072-0.080 s`.


## Measurement caveat

The dev box is a 3.9 GB WSL2 VM with 8 vCPUs on a shared, noisy host; absolute
times here are roughly 2-3x what a quiet 12-thread scoring box should produce,
and they vary by a factor of two between runs. Only *interleaved* comparisons on
the same host, as above, are meaningful. The scoring numbers must come from a
live-board run.

## Step 2 details

- **Heap.** Comparison heap replaced with a 65-bucket radix heap over the
  monotone keys Dijkstra produces (`bucket = highest differing bit vs last pop`).
  Single-core Dijkstra on `full`: 29 s -> 6.8 s.
- **Per-node state.** `visited[]` + `dist[]` (two cache lines per neighbour)
  replaced by one 64-bit word per node holding `(stamp << shift) | distance`.
  Exact because every candidate is below `n * 2^32`; `shift` is computed from `n`
  with two bits of slack. One cache line per neighbour, no clearing between
  queries, and 14 MB per worker instead of 17 MB.
- **Build.** Both passes over the `mmap`ed text run on line-aligned chunks in
  parallel, with per-chunk degree counters and an atomic cursor for the scatter.
  Graph build on `full`: ~0.9 s serial -> ~0.35 s.
- **Exact shortcuts.** `s == t`, source with no out-edges, target with no
  in-edges are answered in O(1). The in-degree test is applied only to query
  targets, via a bitmap built during the scatter, instead of maintaining a
  full in-degree array. This removed two whole-graph scans (about 7 s of the
  old run) caused by the "unreachable" queries.
- **Prefetching.** `tag[]` is prefetched four edges ahead in the relaxation loop.
- **Rejected:** bidirectional Dijkstra. 1.8x fewer settled nodes, no wall-clock
  gain; the doubled random-access footprint is visible in a padded-state control
  experiment. See `README.md`.
- **Rejected:** a parallel BFS for the node order (pthread barrier per level, and
  a barrier-free work-bag version). Both lost to the 0.1 s serial BFS because the
  order inside a BFS level is what carries the locality. See `README.md`.

## Step 3 details

- **Relabelling.** `bfs_order()` builds a breadth-first order from the
  highest-out-degree node; `permute_range()` rewrites the CSR in that order and
  `pos[]` maps the query endpoints through the same permutation. Answers are
  unchanged by construction, and all three hashes match. `head2[]` is computed by
  `deg_range()` in parallel (the random `head[order[nu]]` probes dominate) and then
  prefix-summed serially.
- **Shared degree counters.** Pass 1 used to keep one `n`-entry counter array per
  thread (7 MB x threads); one shared array with relaxed atomic adds measured the
  same and cut peak RSS from 200 MB to 177 MB, which also allowed 16 build
  threads without a memory penalty on larger machines.
- **Rejected on the way:** a single-branch relax (`(x >> shift) != cur ||
  nd < (x & mask)`) looked 2x slower at first, but granular re-profiling showed
  the query phase unchanged; the measurement was host noise. The two-branch form
  is kept because it is the one that was actually verified.


## Correctness gates run

- `full`, seed 1: `distances.txt` SHA-256
  `a23dbdf0a3e49cc8901310a9ee1dc6a9a569a85297b6da75c2a7002cb2ed277e`.
- `sample`, seed 1: `a4c76ed030bc7e365db33191f6cf74708fba7f25eb48d64c8c4279f2e57046a0`.
- `tiny`, seed 1: `ecaca77d3221fe3928109da36480cfdb39f288f2e57ecbc61d3d7706d5cb4bd5`.
- 420 random graphs, plus long chains whose answers exceed 2^32-1, compared
  against an independent Python Dijkstra (zero weights, `2^32-1` weights,
  self-loops, isolated nodes, `n = 0..3` edge cases). All match.
- Forced stamp rollover every 2 and every 5 queries: same hashes.
- AddressSanitizer + UndefinedBehaviorSanitizer on `tiny` and `sample`: clean.
- `-O2 -march=x86-64` build (portability, no AVX/BMI): same hashes.
