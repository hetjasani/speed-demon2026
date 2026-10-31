# Track C: Join

## Approach
Our solution is written in pure C (C99/POSIX) with 0 external dependencies:
1. **Memory-Mapped I/O:** Both files are mapped into memory via `mmap` (with sequential readahead), so the payload is never copied into user buffers.
2. **Hostile CSV Lexing:** A custom parser handles Excel-dialect quotes (`""`), embedded newlines, mixed CRLF/LF, and UTF-8 BOM markers at the start of any record. `memchr` finds closing quotes; escaped names are only unescaped into a scratch buffer when they actually contain `""`.
3. **Integer Cents:** Monetary amounts are parsed directly into signed 64-bit integer cents to prevent floating-point inaccuracies.
4. **Direct Region Lookup:** `regions.csv` is parsed once into a dense `uint16_t` array indexed by `region_id`, and three-letter country codes map through a 26³ direct index. A record is one array load, not a hash lookup.
5. **Parallel Work Stealing:** The record body is split at unquoted newlines only. Chunk boundaries are found with AVX2/SSE2/SWAR scans that track quote parity ~32 bytes at a time, and threads pull chunks from an atomic counter so slow cores do not leave a straggler tail.
6. **Zero-Copy Aggregation:** Each worker keeps per-country aggregates and an open-addressing table of distinct names. Names are interned once into a per-worker arena; the merge phase points the final tables straight at those immutable arenas, then each thread merges a disjoint slice of countries with no locking.

Thread count comes from `sched_getaffinity`, so a container pinned to a CPU subset does not oversubscribe the host.

## Dependencies
None (C Standard Library / POSIX only). Builds with the provided `Dockerfile`:
`gcc -O3 -march=native -flto -fno-plt -fomit-frame-pointer -pthread`.
