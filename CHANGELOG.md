# Speed Demon 2026 — Engineering Changelog

## [0.6.0] - 2026-11-01
### Added
- Upgraded Track A (Ingest) engine to Memory Arena Architecture in `track-a/main.cpp`.
- Implemented Two-Pass streaming: Pass 1 parses hours, endpoints, counts, and errors into L2-resident arrays (589 KB per thread) with zero latency parsing and zero vector allocations.
- Flattened group latencies into a single contiguous `uint32_t` array `all_latencies` (12.4 MB on full, 1.24 MB on sample), freeing Pass 1 count/error buffers prior to Pass 2.
- Direct Pass 2 streaming into `all_latencies[write_offsets[t * G + gid]++]` without mutexes, atomic operations, or dynamic heap reallocation.
- Compressed source code from 605 lines to **291 lines** (beating Kirk Mates' 294 lines), maximizing efficiency score coefficient $K$.
- Slashed peak memory consumption from 123 MB to **15.6 MB** in container environment.
### Verified
- Exact SHA-256 match on `data/a_tiny`: `C24FA507C963EAEAC9224FD8435019B4F4E8C002ECE7E32F316681A3957EB92F`.
- Exact SHA-256 match on `data/a_sample`: `CA6311BBEB536FA55D85265BF6FD3D24CC9586F13DE1AC73BD2A23AC3893AAC3`.
- Full test suite `speed-demon-2026/tests/test_a_*.py`: 15 tests — **ALL PASS**.
- Measured wall time on `data/a_sample`: **35.69 ms** local (~18 ms on dedicated Falkenstein bare metal).
- Cleaned up all `.exe` artifacts.

## [0.5.0] - 2026-11-01
### Added
- High-performance zero-dependency C++20 engine in `track-b/main.cpp` for Track B (Index).
- Query-directed vocabulary indexing: parses `queries.txt` first to map only active query terms (11,430 terms vs 378k corpus words in `sample`), filtering out unneeded terms in $O(1)$ during corpus ingest without memory allocation.
- Zero-copy memory mapping for both `docs.txt` and `queries.txt` via `MapViewOfFile` / `mmap`.
- Persistent 16-worker thread pool synchronized with `std::barrier` across parallel document boundary scanning, document tokenization & bitset indexing, and query evaluation.
- Contention-free 64-document aligned bitset partitioning: each worker thread exclusively writes to non-overlapping 64-bit words, eliminating mutexes, atomic instructions, and false sharing.
- AVX2 bitwise SIMD accumulator evaluating left-to-right boolean operators (`AND`, `OR`, `NOT`) in ~12.5 ns per operator.
- 16-bit popcount & bit index sum lookup table (`LUT16`) computing match count and exact 64-bit ID sum in 4 lookups per 64-bit word without branches.
- Anchor-pruned candidate phrase verification: intersects term bitsets (averaging only 1.97 candidates per query) and performs on-the-fly exact token sequence verification against raw mapped document text.
- Allowlisted two-stage Dockerfile (`gcc:16` build stage, `debian:13-slim` runtime stage) and `.speed-demon.toml` configured with `team = "Nomad Labs"`, `language = "C++"`, `dependency_count = 0`, validated with `dockerfile_check.py` and `dependencies.py`.
### Verified
- Exact SHA-256 match on `data/b_tiny`: `DCDAD7CEDFC07EB66ECCED1861CC91D9017E004C56CBE4F38E9413B62B17FF70`.
- Exact SHA-256 match on `data/b_sample`: `EEEEC1E9319DA98A6F3C664C78F93E7078FD603C25C2BB3027174E81E3638A8F`.
- Full unit test suite `speed-demon-2026/tests/test_b_*.py`: 15 tests — **ALL PASS**.
- Achieved **25.05 ms** internal engine execution time (**39.0 ms** process wall time) on `data/b_sample` (54.0x speedup over reference baseline 1,350 ms; 56.2x vs local Python 2,193 ms).
- Peak anonymous memory footprint: ~36 MB, complying strictly with the `<40MB RAM` ceiling.

## [0.4.0] - 2026-11-01
### Added
- High-performance zero-dependency C++20 engine in `track-d/main.cpp` for Track D (Traverse).
- Memory-mapped zero-copy file streaming for `graph.txt` and `queries.txt` with `MapViewOfFile` / `mmap`.
- Compact Compressed Sparse Row (CSR) graph layout (`head[n+1]` index array and `edges[m]` contiguous edge array of 8-byte `{uint32_t v; uint32_t w;}`).
- Concurrent worker thread initialization: 16 worker threads spawned immediately after parsing graph header (`n, m`), pre-allocating Dijkstra query buffers in parallel during edge text parsing and CSR construction, completely hiding thread creation latency.
- Epoch-based $O(1)$ Dijkstra state reset (`seen[v] != cur_epoch`) with persistent `dist[n]` and `seen[n]`, eliminating 22 MB of `memset` memory writes per query.
- Custom cache-aligned 64-byte 4-ary MinHeap (`QuadHeap`) with hole propagation, halving tree depth ($log_4 N$) and fitting 4 sibling children in a single CPU cache line with internal buffer retention across queries.
- Dynamic atomic work stealing (`std::atomic<size_t> query_cursor.fetch_add(1)`) providing optimal load balancing across 16 threads without query stragglers.
- $O(1)$ reachability pruning: immediate answers for `s == t` (0) and disconnected nodes (`out_degree[s] == 0` or `in_degree[t] == 0` -> -1), preventing expensive full-graph traversals.
- Fast bounds-free integer parsing and 2-digit LUT integer formatter writing directly to binary LF-terminated output buffer.
- Allowlisted two-stage Dockerfile (`gcc:16` build stage, `debian:13-slim` runtime stage) and `.speed-demon.toml` configured with `team = "Nomad Labs"`, `language = "C++"`, `dependency_count = 0`, validated against `check.py`.
### Verified
- Exact SHA-256 match on `data/d_tiny`: `ECACA77D3221FE3928109DA36480CFDB39F288F2E57ECBC61D3D7706D5CB4BD5`.
- Exact SHA-256 match on `data/d_sample`: `A4C76ED030BC7E365DB33191F6CF74708FBA7F25EB48D64C8C4279F2E57046A0`.
- Verified across all unit test cases (worked example, parallel lighter/heavier edges, 3-way duplicates, 0-weight cycles, unreachable nodes, 64-bit distances, multi-seeds 2, 7, 42, 99).
- Achieved **62.44 ms** execution time on `data/d_sample` (60.6x speedup over Python reference baseline 3,787 ms; 59.4x vs measured Python 3,708.76 ms).

## [0.3.0] - 2026-11-01
### Added
- High-performance zero-dependency C++20 engine in `track-c/main.cpp` for Track C (Join).
- Memory-mapped zero-copy file streaming for `records.csv` and `regions.csv` with `MapViewOfFile` / `mmap`.
- Single-pass `regions.csv` parsing with zero-allocation BSS direct lookup table `region_to_country[10000000]`, using `0` for `_UNMATCHED` and `1..num_countries` for sorted country codes.
- Parallel CRLF record-boundary chunking with `std::barrier`, safely splitting hostile CSV files across 16 threads without quote-boundary corruption.
- Single-pass on-the-fly unescaping, byte length accumulation, and 64-bit FNV-1a hashing for quoted/unquoted `name` and `note` fields with zero intermediate string allocations.
- Full compliance with UTF-8 BOM (`\xef\xbb\xbf`) detection at file start and mid-file boundaries after both CRLF and LF terminators.
- Exact signed integer cents arithmetic (`cents = whole * 100 + frac`) without floating-point math.
- Pre-allocated 2048-slot contiguous hash pool per thread and compact unique key tracking for fast lock-free country reduction.
- Fast LUT-based integer and signed decimal cents formatter.
- Allowlisted two-stage Dockerfile (`gcc:16` build stage, `debian:13-slim` runtime stage) and `.speed-demon.toml` configured with `team = "Nomad Labs"`, `language = "C++"`, `dependency_count = 0`, validated against `dockerfile_check.py`.
### Verified
- Exact SHA-256 match on `data/c_tiny`: `E4E43BC39920E9D446C3FCD700FDD35B3959E9495C6AAA738491DF38A1AED733`.
- Exact SHA-256 match on `data/c_sample`: `9035FED36823232DD41AE2D1B9D6D21AA7C73F49D8E95647E953A89D5EBE9A66`.
- Verified on all test suites and hand-crafted edge cases: `WORKED_RECORDS`, `RULES_RECORDS` (leading BOM, final LF, final CRLF), and `test_c_*.py` (14/14 tests pass).
- Achieved **33.45 ms** execution time on `data/c_sample` (35.3x speedup over Python reference baseline 1,181 ms; 40.8x vs measured Python).

## [0.2.0] - 2026-11-01
### Added
- High-performance zero-dependency C++20 engine in `track-a/main.cpp` for Track A (Ingest).
- Memory-mapped zero-copy line ingest with `MapViewOfFile` / `mmap`.
- Lock-free worker thread pool with `std::barrier` synchronized across chunk parsing, group scatter, and percentile calculations.
- Direct mathematical RFC 3339 hour index arithmetic (`hid = day_offset * 24 + h - 18`) eliminating date parsing overhead.
- L1-cache resident 8192-slot open-addressing endpoint hash table with unrolled 64-bit FNV hashing.
- Sparse active group tracking and contiguous latency flattening for linear-time $O(n)$ `std::nth_element` nearest-rank percentiles.
- Fast 2-digit lookup table decimal formatter.
- Allowlisted two-stage Dockerfile (`gcc:16` build stage, `debian:13-slim` runtime stage) and `.speed-demon.toml` configured with `team = "Nomad Labs"`, `language = "C++"`, `dependency_count = 0`.
### Verified
- Exact SHA-256 match on `data/a_tiny`: `C24FA507C963EAEAC9224FD8435019B4F4E8C002ECE7E32F316681A3957EB92F`.
- Exact SHA-256 match on `data/a_sample`: `CA6311BBEB536FA55D85265BF6FD3D24CC9586F13DE1AC73BD2A23AC3893AAC3`.
- Achieved **38.38 ms** execution time on `data/a_sample` (75.5x speedup over Python reference baseline).

## [0.1.0] - 2026-11-01
### Added
- Cloned starter template and official 2026 specs repository into `I:\Career\Speed Demon\`.
- Installed and verified portable 64-bit GCC 14.1.0 toolchain with C++20 and thread support (`std::thread::hardware_concurrency() = 16`).
- Generated Track A `tiny` and `sample` benchmark datasets with Seed 1.
- Benchmarked naive Python baseline on `data/a_sample` (2.898 seconds) and recorded ground-truth SHA-256 output hashes.
- Established master architecture documentation: `PROJECT_CONTEXT.md`, `ROADMAP.md`, and `PERF.md`.
