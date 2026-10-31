# Speed Demon 2026 — Engineering Changelog

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
