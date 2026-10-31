# Track C: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID / Commit | T (s) | M (MB) | Points | K |
|---|---|---|---|---|---|---|
| 0 | The reference, unchanged | - | 14.44 | 367 | 0 | 1.000 |
| 1 | Pure C single-pass parser with mmap & custom arena | 59b74de | 0.991 | 398 | 23.873 | 3.002 |

### Step 1 Technical Details
- **Baseline Bottleneck:** The Python reference implementation took 14.44s due to Python `csv.reader` overhead, dynamic string allocation, and object creation for each record.
- **Changes Implemented:**
  - Complete rewrite in pure C99 with zero external dependencies ($D = 1.0$).
  - Memory-mapped file I/O (`mmap`) replacing buffered file reading.
  - Custom RFC 4180 quote-aware CSV state machine with BOM stripping.
  - Chunked arena allocation for deduplicating unique names.
  - Exact integer cents arithmetic avoiding float rounding errors.
- **Result:** Execution time dropped from 14.44s to **0.991s** (~14.5× faster).