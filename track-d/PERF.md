# Track D: performance log

Every step from the reference to the final version. Board run IDs are recorded
when a step is submitted; the numbers below are measured locally, so the run ID
column is `local` until a live-board run replaces it.

| Step | Change | Run ID | T (s) | M (MB) |
|---|---|---|---|---|
| 0 | The reference, unchanged (spec, M4 Pro) | - | 43.0 - 46.8 | 915 |
| 1 | Original pure-C version: CSR + 4-ary heap + 16 threads | local | 5.64 | 301 |
| 2 | Radix heap, packed epoch+distance word, parallel two-pass CSR, exact shortcuts, prefetch | local | 2.03 | 200 |

Steps 1 and 2 are the same binary pair, alternating run 1 / run 2 on one machine
(5 rounds, `min` of each; medians 6.48 s and 2.49 s). On `sample`: 0.18 s and
0.09 s. Peak RSS is from `/usr/bin/time -v`-style `%M`.

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
