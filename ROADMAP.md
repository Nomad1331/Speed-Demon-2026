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

---

## Phase 3: Track C (Join) Hostile CSV Engine
- [x] **Iteration 0: Correctness Engine:** Compliant CSV RFC state machine handling embedded commas, newlines, UTF-8 BOM, and quotes 🟢
- [x] **Iteration 1: Aggregation & Join:** Zero-allocation BSS flat array region lookup (`region_to_country[10000000]`) and exact integer cents accumulation 🟢
- [x] **Iteration 2: Parallel Stream Processing:** Multi-threaded CRLF boundary chunking with `std::barrier`, on-the-fly zero-copy unescaping and FNV-1a hashing 🟢
- [x] **Iteration 3: Reduction & Formatting:** Lock-free parallel country reduction and unrolled LUT integer/cents formatting 🟢
- [x] **Iteration 4: Container Packaging:** Multi-stage Dockerfile matching GCC 16 allowlist and zero-dependency `.speed-demon.toml` validated with `dockerfile_check.py` 🟢
- [x] **Iteration 5: Verification:** Byte-identical SHA-256 match on `tiny` and `sample` datasets, achieving 33.45 ms runtime (35.3x speedup) 🟢

---

## Phase 4: Track D (Traverse) Graph Shortest-Paths
- [ ] **Iteration 0: Fast CSR Representation:** Compact adjacency array representation for 1.74M nodes and 5M edges 🔴
- [ ] **Iteration 1: Bidirectional Dijkstra / CH:** Fast query execution for 2,000 source-target pairs 🔴
- [ ] **Iteration 2: Multi-threaded Batch Execution:** Distribute queries across 12 worker threads 🔴
- [ ] **Iteration 3: Verification:** SHA-256 validation 🔴

---

## Phase 5: Submission & Live Board Telemetry
- [ ] Link GitHub repository to `speed-demon-board` GitHub App 🔴
- [ ] Register team submission form on Tally (`https://tally.so/r/Ar7g9z`) 🔴
- [ ] Push to `speed-demon` branch and monitor automated Falkenstein board runs 🔴
- [ ] Complete `PERF.md` optimization log for all 3 entered tracks (8% jury weight) 🔴
