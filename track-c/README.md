# Track C: Join

## Approach
Our solution is written in pure C (C99/POSIX) with 0 external dependencies:
1. **Memory-Mapped I/O:** `records.csv` is mapped into memory via `mmap` to avoid buffer copying.
2. **Hostile CSV Lexing:** A custom parser handles Excel-dialect quotes (`""`), embedded newlines, and strips UTF-8 BOM markers.
3. **Integer Cents:** Monetary amounts are parsed directly into signed 64-bit integer cents to prevent floating-point inaccuracies.
4. **Hash Table & Arena:** Country aggregates and regions are stored in open-addressing hash tables with FNV-1a hashing. Unique country names are deduplicated using a lightweight chunk arena.

## Dependencies
None (C Standard Library only).