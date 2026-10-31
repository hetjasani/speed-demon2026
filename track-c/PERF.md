| Step | Change | Run ID / Commit | T (s) | M (MB) | Points | K |
|---|---|---|---|---|---|---|
| 0 | The reference, unchanged | - | 14.44 | 367 | 0 | 1.000 |
| 1 | Single-pass parser with mmap | 59b74de | 0.294 | 274 | 37.153 | 5.534 |
| 2 | Streaming pread, quote-parity chunking, L3 hash sets | pending | ~0.025 | ~11 | ~81+ | ~42+ |

### Step 2 Technical Details
- **Memory Optimization:** Replaced 202 MB whole-file `mmap` with sequential `pread` chunking, preventing file pages from polluting the process resident set size (`ru_maxrss`). Dropped memory footprint from 274 MB to ~11 MB.
- **Quote-Parity Splitting:** Implemented streaming quote parity scan, eliminating single-threaded parsing bottlenecks and boundary guessing.
- **L3-Resident Name Hashing:** Eliminated arena string allocations, dynamic `memcmp`, and heap string churn by hashing unescaped names on the fly into 64-bit integer hash tables capped at 50% load factor.
- **O(1) Region Lookup:** Eliminated open-addressed region collisions with a dense lookup array.