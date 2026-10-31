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
| 4 | Parse the graph once into an edge buffer, fill the CSR by node range | local | 0.80 | 177 |
| 5 | Count degrees by node range, fold the degree gather into the BFS, bitmap visited | local | 0.77 | 177 |

Steps 1 and 2 are the same binary pair, alternating run 1 / run 2 on one machine
(5 rounds, `min` of each; medians 6.48 s and 2.49 s). On `sample`: 0.18 s and
0.09 s. Peak RSS is from `/usr/bin/time -v`-style `%M`.

Steps 3 and 4 were measured against step 2 interleaved on `full`. The last
comparison (3 rounds, quiet host, load average 0.8-1.4, same `full` hash on both
sides) is the headline:

| Round | Version | Wall | User | Query phase | Peak RSS |
|---|---|---|---|---|---|
| 1 | step 2 (shipped, 46.128) | 1.52 s | 10.77 s | 1.22 s | 200 MB |
| 1 | step 4 | 0.79 s | 4.89 s | 0.45 s | 177 MB |
| 2 | step 2 | 1.35 s | 10.57 s | 1.08 s | 200 MB |
| 2 | step 4 | 0.82 s | 4.98 s | 0.47 s | 177 MB |
| 3 | step 2 | 1.30 s | 10.37 s | 1.09 s | 200 MB |
| 3 | step 4 | 0.81 s | 5.13 s | 0.48 s | 177 MB |

So 1.6x wall and 2.1x less CPU, with the query phase 2.4x cheaper. Per phase on
step 4 (`full`): `build=0.18-0.20, bfs+deg=0.13, perm=0.04, rest=0.01,
query=0.45-0.48`, i.e. preprocessing and queries now cost about the same.
Query-only, 200 queries on the full graph, `min` of 4 interleaved runs: step 2
`0.144-0.187 s`, step 4 `0.071-0.078 s`.


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


## Step 5 details

The build was profiled one phase at a time; the parse pass (0.16-0.20 s) and the
serial BFS (0.13 s) were the two items that mattered.

- **No degree counting while parsing.** Pass 1 used a shared `uint32_t[1.7 M]`
  degree array with a relaxed atomic add per edge. Measured on `full`: parse with
  that atomic 0.154-0.218 s, with a plain `cnt[u]++` 0.121-0.189 s, with the
  counter folded into 8 K entries 0.089-0.139 s, and with no counting at all
  0.065-0.075 s. So the *random 7 MB access*, not the atomic, was ~0.09 s.
  Degrees are now counted in a second pass by node range, into a per-thread
  slice of `n/threads` counters (870 KB, L2-resident): that pass costs 0.02 s.
- **SoA parse buffer.** The parsed rows are kept as three flat arrays
  (`tu/tv/tw`) instead of interleaved 12-byte rows. The count pass then reads
  only `tu` (20 MB) instead of pulling a 60 MB line stream to use one word of
  every row, and the fill pass only touches `tv/tw` for the rows it keeps.
  Total pass-2/3 traffic went from ~480 MB to ~360 MB.
- **The fill keeps local cursors.** The per-thread degree slice is already the
  local prefix sum, so it doubles as the cursor array: no shared cursor, and
  the offsets are written cache-hot.
- **Degree gather folded into the BFS.** `bfs_order()` reads `head[u]` and
  `head[u+1]` anyway to walk the edges, so `head2[i+1] = e_end - e` is free
  there; the separate parallel `deg_range()` pass over random
  `head[order[nu]]` is gone.
- **Bitmap visited in the BFS.** The BFS used `pos[v] == UINT32_MAX` as its
  visited test, i.e. a random probe of a 7 MB table per out-edge. It now uses a
  1-bit-per-node bitmap (218 KB, stays in L2), and `pos[]` is filled afterwards
  by a parallel `pos[order[i]] = i` pass (0.002 s). BFS: 0.125 s -> 0.073 s.

Interleaved A/B on `full`, 3 rounds (`pre` is everything before the queries):

| Round | Version | pre | Wall | User | Query | Peak RSS |
|---|---|---|---|---|---|---|
| 1 | step 4 | 0.435 s | 0.95 s | 5.21 s | 0.521 s | 177 MB |
| 1 | step 5 | 0.267 s | 0.75 s | 4.60 s | 0.480 s | 177 MB |
| 2 | step 4 | 0.351 s | 0.84 s | 4.91 s | 0.486 s | 177 MB |
| 2 | step 5 | 0.272 s | 0.77 s | 4.65 s | 0.492 s | 177 MB |
| 3 | step 4 | 0.379 s | 0.84 s | 4.83 s | 0.459 s | 177 MB |
| 3 | step 5 | 0.284 s | 0.82 s | 4.77 s | 0.523 s | 177 MB |

Per phase on step 5 (`full`, first run includes the query-file parse):
`setup=0.06, parse=0.05, count=0.02, fill=0.07, bfs=0.073, pos=0.002,
scan=0.003, perm=0.045, query=0.45-0.50`. Preprocessing is just under a third of
the run now, and the queries are half of it.

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
