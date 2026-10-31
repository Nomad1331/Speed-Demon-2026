# Track C: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged (Python 3.13) | local-ref | 1.181 | 185 | 0 |
| 1 | C++20 zero-copy parallel engine, BSS flat array region lookup, multi-threaded CRLF boundary chunking with `std::barrier`, on-the-fly FNV-1a unescape hashing, and lock-free parallel country merge | local-cpp-v1 | 0.033 | 32 | 93.8 |
