# Speed Demon 2026 — Sprint Roadmap

## Status Legend
- 🔴 Not Started
- 🟡 In Progress
- 🟢 Completed & Verified

---

## Phase 1: Environment & Toolchain Setup
- [x] Clone starter template & official harness specs into `I:\Career\Speed Demon\` 🟢
- [x] Configure portable 64-bit GCC 14.1.0 toolchain with C++20 and thread support 🟢
- [x] Establish local data generation harness (`data/a_tiny`, `data/a_sample`) 🟢
- [x] Establish reference ground truth hashes & baseline metrics 🟢

---

## Phase 2: Track A (Ingest) Optimization Loop
- [x] **Iteration 0: Baseline Architecture:** Single-threaded zero-copy line parsing & hash table grouping 🟢
- [x] **Iteration 1: High-Throughput I/O:** Memory-mapped chunked multi-threaded file reading with `std::barrier` 🟢
- [x] **Iteration 2: Zero-Parsing Optimization:** Fast RFC 3339 hour formula (`hid = day_offset * 24 + h - 18`), custom fast integer parser for latency & status 🟢
- [x] **Iteration 3: Algorithmic Percentiles:** Contiguous latency flattening, `std::nth_element` selection for nearest-rank p50/p95/p99 🟢
- [x] **Iteration 4: Container Packaging:** Multi-stage Dockerfile matching GCC 16 allowlist and zero-dependency `.speed-demon.toml` 🟢
- [x] **Iteration 5: Verification:** Byte-identical SHA-256 match on `tiny` and `sample` datasets, achieving 38.38 ms runtime (75.5x speedup) 🟢
- [x] **Iteration 6: Single-Pass Ingest & Pass 2 Elimination:** Eliminated second file pass over mmap, caching 8-byte `LocalReq` in L3, evicting file via `MADV_DONTNEED`, in-memory scatter into `all_latencies`, wall time $\le 31$ ms, 286 SLOC 🟢
- [x] **Iteration 7: Zero-Syscall Stream Ingest & Disjoint 2D Scatter Overhaul:** Single-pass packed 64-bit ingest (`((ep_id<<48)|(hid<<32)|lat)`), zero syscalls in inner loop, Thread 0 disjoint `write_offsets[num_threads][G]` calculation, immediate physical file unmapping/closing at Barrier 1, lock-free deterministic scatter into contiguous `all_latencies`, cascaded `std::nth_element` quantile pipeline, compressed to 276 SLOC (strictly $\le 280$). Verified byte-identical SHA-256 on tiny and sample; all 15 unit tests pass 🟢

---

## Phase 3: Track B (Index) Query-Directed Inverted Index
- [x] **Iteration 0: Query-Directed Filtering:** Parse queries first to index only the 11,430 query terms out of 378k corpus words 🟢
- [x] **Iteration 1: Zero-Contention Thread Pool:** Single-spawn persistent 16-worker thread pool with `std::barrier` and 64-document aligned bitset partitioning (zero locks, zero word contention) 🟢
- [x] **Iteration 2: SIMD Accumulator & LUT Tally:** AVX2 vector bitwise evaluation with 16-bit popcount & sum lookup table (`LUT16`) 🟢
- [x] **Iteration 3: Anchor-Pruned Phrase Verification:** Bitset candidate intersection followed by on-the-fly exact token scan on candidate docs 🟢
- [x] **Iteration 4: Packaging & Validation:** Multi-stage GCC 16 Dockerfile validated with `dockerfile_check.py` and zero-dependency `.speed-demon.toml` 🟢
- [x] **Iteration 5: Verification:** Byte-identical SHA-256 match on `tiny` and `sample`, achieving 25.05 ms runtime (54.0x speedup) 🟢
- [x] **Iteration 6: Sparse/Dense Hybrid Posting Overhaul:** Adaptive thresholding (`N / 32`), compact contiguous sorted `uint32_t` postings for rare terms, two-pointer linear intersection in 15 ns, streaming candidate verification. Squeezed full-scale RAM from 2,405 MB down to ~34 MB (<42 MB target), achieving 33.78 ms wall time and passing all 15 unit tests 🟢

---

## Phase 4: Track C (Join) Hostile CSV Engine
- [x] **Iteration 0: Correctness Engine:** Compliant CSV RFC state machine handling embedded commas, newlines, UTF-8 BOM, and quotes 🟢
- [x] **Iteration 1: Aggregation & Join:** Zero-allocation BSS flat array region lookup and exact integer cents accumulation 🟢
- [x] **Iteration 2: Parallel Stream Processing:** Multi-threaded CRLF boundary chunking with `std::barrier`, on-the-fly zero-copy unescaping and FNV-1a hashing 🟢
- [x] **Iteration 3: Turbo Memory Squeeze:** Sized 1 MB compact region lookup table, single per-thread 64k filter table, direct vector merge-sort deduplication, and fused line formatting 🟢
- [x] **Iteration 4: SLOC Compression:** Squeezed code to 332 lines matching NickTrapp's 303 lines 🟢
- [x] **Iteration 5: Container Packaging & Validation:** Multi-stage Dockerfile matching GCC 16 allowlist and zero-dependency `.speed-demon.toml` validated with `dockerfile_check.py` 🟢
- [x] **Iteration 6: Verification:** Byte-identical SHA-256 match on `tiny` and `sample`, achieving 21.01 ms runtime (~18 ms on scoring box, <13 MB RAM, projected 84+ pts) 🟢
- [x] **Iteration 7: Hardware CRC32 + Zero-Alloc Packed uint64_t Pipeline:** Replaced serial FNV-1a with hardware CRC32 instruction (`_mm_crc32_u64`), packed `((cid << 32) | crc)` into a single flat `uint64_t` buffer per thread, eliminated all heap allocations and 241 dynamic vectors per thread. Compressed code from 332 to 290 SLOC (strictly $\le 300$). Verified byte-identical SHA-256 on tiny and sample. 🟢

---

## Phase 5: Track D (Traverse) Graph Shortest-Paths
- [x] **Iteration 0: Fast CSR Representation:** Compact zero-copy Compressed Sparse Row representation (`head[n+1]`, `edges[m]`) with prefix sums 🟢
- [x] **Iteration 1: Thread Pre-Warming & O(1) Epoch Reset:** Spawn 16 worker threads concurrently during edge parse; persistent `dist[n]` and `seen[n]` with epoch increments (zero memset) 🟢
- [x] **Iteration 2: 4-ary MinHeap & Dynamic Work Stealing:** Custom cache-aligned 64-byte `QuadHeap` with hole propagation and atomic dynamic work-stealing load balancing 🟢
- [x] **Iteration 3: Reachability Pruning & Fast Output:** O(1) pruning for `s == t`, `out_degree == 0`, and `in_degree == 0`; 2-digit LUT output formatting 🟢
- [x] **Iteration 4: Container Packaging:** Multi-stage Dockerfile matching GCC 16 allowlist and zero-dependency `.speed-demon.toml` validated with `check.py` 🟢
- [x] **Iteration 5: Verification:** Byte-identical SHA-256 match on `tiny` and `sample` datasets, achieving 62.44 ms runtime (60.6x speedup) 🟢

---

## Phase 6: Submission & Live Board Telemetry
- [ ] Link GitHub repository to `speed-demon-board` GitHub App 🔴
- [ ] Register team submission form on Tally (`https://tally.so/r/Ar7g9z`) 🔴
- [ ] Push to `speed-demon` branch and monitor automated Falkenstein board runs 🔴
- [ ] Complete `PERF.md` optimization log for all 3 entered tracks (8% jury weight) 🔴
