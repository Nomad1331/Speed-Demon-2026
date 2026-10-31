# Track A: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged (Python 3.13) | local-ref | 2.68 | 180 | 0 |
| 1 | C++20 zero-copy ingest, thread pool with `std::barrier`, fast RFC 3339 hour formula, open-addressing endpoint hashing, SIMD-friendly scatter and parallel `std::nth_element` percentiles | local-cpp-v1 | 0.038 | 24 | 92.5 |
| 2 | Memory Arena Architecture: Two-pass zero-allocation stream, single contiguous flat latency array (12.4 MB), freed Pass 1 memory, compressed to 291 lines matching Kirk Mates architecture | local-cpp-v2 | 0.035 | 15.6 | 88.4 |
