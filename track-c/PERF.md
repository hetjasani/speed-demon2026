# Track C: performance log

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | - | 14.44 | 367 | 0 |
| 1 | Pure C single-pass parser with mmap & custom arena | <PUT_RUN_ID_HERE> | <T> | <M> | <PTS> |

### Step 1 Details
- **Bottlenecks in reference:** Python's `csv` module and dynamic object/dict allocations took 14.4s and 367 MB.
- **Our changes:**
  - Used POSIX `mmap` for zero-copy file access.
  - Implemented a custom CSV state machine handling RFC 4180 quotes (`""`), embedded newlines, and BOM markers without regex.
  - Used an arena allocator to eliminate `malloc` overhead during string deduplication.
  - Zero external dependencies ($D = 1$).