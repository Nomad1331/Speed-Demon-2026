# Performance Engineering & Optimization Log — Speed Demon 2026

This log documents every experimental iteration, architectural rationale, measurement result, and recurrent feedback loop across all evaluated tracks.

---

## Track A: Ingest

### Baseline: Naive Python Reference (`track-a/a_ingest.py`)
- **Dataset:** `data/a_sample` (310,000 log records, 23.8 MB, Seed 1)
- **Wall Time ($T_{\text{ref}}$):** 2,898 ms (~2.9 seconds; warm run: 2,680 ms)
- **Projected Full Time (3.1M records):** ~29,000 ms (~29 seconds)
- **Memory Footprint:** Python process allocating dynamic strings, dicts of lists of ints (~180 MB RSS)
- **Bottlenecks Identified:**
  1. `datetime.strptime(ts, "%Y-%m-%dT%H:%M:%S.%f%z")` called 3.1 million times (massive CPU overhead).
  2. String splitting `line.rstrip().split(" ")` creates 5 heap objects per line.
  3. Dynamic `latencies[key].append(int(latency_us))` causes frequent vector reallocations.
  4. Full sorting of all group latencies (`sorted(latencies[key])`) instead of efficient selection.
- **Reference Output SHA-256 (`tiny`):**
  `C24FA507C963EAEAC9224FD8435019B4F4E8C002ECE7E32F316681A3957EB92F`
- **Reference Output SHA-256 (`sample`):**
  `CA6311BBEB536FA55D85265BF6FD3D24CC9586F13DE1AC73BD2A23AC3893AAC3`

---

## Optimization Iteration Log: Track A

### Cycle 1: High-Performance Multi-Threaded C++20 Zero-Copy Engine (`track-a/main.cpp`)
- **Hypothesis:** Memory mapping, direct pointer parsing, lock-free thread pooling with `std::barrier`, fast RFC 3339 hour arithmetic, open-addressing endpoint hashing, and two-pass contiguous cache layout will eliminate all memory allocations from the hot path and achieve sub-40 ms execution on `sample`.
- **Key Techniques Implemented:**
  1. **Zero-Copy Memory-Mapped I/O:** `MapViewOfFile` (Win32) / `mmap` (POSIX) eliminates heap copy buffers and paging overhead.
  2. **Thread Pool with `std::barrier`:** Replaces repeated OS thread creation/destruction across 5 phases with a single spawn/join barrier pool across 16 logical hardware threads.
  3. **Direct O(1) RFC 3339 Hour Indexing:** Replaced string date parsing with a direct integer formula: `hid = day_offset * 24 + h - 18` mapping the 72-hour window directly into `[0..71]`. Hour strings are precomputed in exact ascending chronological/lexicographical order.
  4. **Open-Addressing Endpoint Hash Table:** 8192-entry power-of-2 linear probing table per thread (45% load factor) residing entirely in L1 data cache.
  5. **Sparse Group Tracking (`active_groups`):** Tracks touched groups during parse, reducing merge loops from $4.7\times 10^6$ to 66,000 iterations (70x reduction).
  6. **Contiguous Scatter & Algorithmic Percentiles:** Flattens 310,000 latencies into a single `uint32_t` vector in exact output order. Computes p50/p95/p99 via `std::nth_element` ($O(n)$) without full sorting for groups $n > 64$.
  7. **2-Digit LUT Output Writer:** Unrolled integer-to-string decimal formatting directly into thread buffers, followed by a single sequential `fwrite`.
- **Measurements on `data/a_sample` (310,000 lines, 23.8 MB):**
  - **Cold Run:** 53.27 ms
  - **Warm Runs (10 iterations):** 40.20 ms, 40.38 ms, 39.96 ms, 38.70 ms, 39.23 ms, 38.38 ms, 40.64 ms, 39.87 ms
  - **Fastest Wall Time ($T$):** **38.38 ms**
  - **Speedup vs Python Baseline (2,898 ms):** **75.5x**
  - **Speedup vs Python Warm (2,680 ms):** **69.8x**
- **Correctness Verification:**
  - `data/a_tiny`: `C24FA507C963EAEAC9224FD8435019B4F4E8C002ECE7E32F316681A3957EB92F` — **EXACT MATCH** (PASS)
  - `data/a_sample`: `CA6311BBEB536FA55D85265BF6FD3D24CC9586F13DE1AC73BD2A23AC3893AAC3` — **EXACT MATCH** (PASS)
- **Score Impact:**
  - $T_{\text{ref}} / T = 2898 / 38.38 = 75.51 \implies (75.51)^{0.50} \approx 8.69\times$
  - Peak RSS: ~24 MB vs ~180 MB $\implies \min(180/24, 50)^{0.20} \approx (7.5)^{0.20} \approx 1.50\times$
  - Dependencies: $D = 1 \implies 1.0\times$
  - Combined Efficiency Coefficient $K \approx 13.0$, yielding projected points $> 90$.

---

## Track C: Join

### Baseline: Naive Python Reference (`track-c/c_join.py`)
- **Dataset:** `data/c_sample` (332,000 records, 19.8 MB `records.csv`, 200,000 regions, 240 countries, 2.2 MB `regions.csv`, Seed 1)
- **Wall Time ($T_{\text{ref}}$):** 1,366 ms (~1.37 seconds; official reference baseline: 1,181 ms)
- **Projected Full Time (3.32M records):** ~14,000 ms (~14 seconds)
- **Memory Footprint:** Python process allocating dynamic strings, dicts, and sets (~185 MB RSS)
- **Hostile CSV & Join Edge Cases Handled:**
  1. UTF-8 BOM (`\xef\xbb\xbf`) at start of file and mid-file at record boundaries after both CRLF and LF.
  2. Mixed CRLF and bare LF line endings.
  3. Quoted fields with commas, escaped quotes (`""`), and embedded bare newlines (`\n`) in `note`.
  4. Empty unquoted and empty quoted `""` fields (both normalized to empty string).
  5. Exact integer cents arithmetic for `amount` (`whole * 100 + frac`, signed, no floats).
  6. Hash join against `regions.csv` with fallback to `_UNMATCHED`.
  7. Exact tracking of distinct names per country with unescaping (`""` -> `"`).
- **Reference Output SHA-256 (`tiny`):**
  `E4E43BC39920E9D446C3FCD700FDD35B3959E9495C6AAA738491DF38A1AED733`
- **Reference Output SHA-256 (`sample`):**
  `9035FED36823232DD41AE2D1B9D6D21AA7C73F49D8E95647E953A89D5EBE9A66`

---

## Optimization Iteration Log: Track C

### Cycle 1: High-Performance Multi-Threaded C++20 Hostile CSV Join Engine (`track-c/main.cpp`)
- **Hypothesis:** Direct BSS region indexing ($O(1)$ flat lookup), parallel CRLF record-boundary chunking, on-the-fly zero-copy unescape FNV-1a hashing, pre-allocated contiguous per-thread hash pools, lock-free parallel country reduction, and custom LUT integer/money formatting will eliminate heap allocations and achieve sub-35 ms execution on `sample`.
- **Key Techniques Implemented:**
  1. **Zero-Copy Memory-Mapped File Ingest:** `MapViewOfFile` / `mmap` zero-copy memory mapping for both `records.csv` and `regions.csv`.
  2. **Zero-Allocation BSS Direct Region Lookup Table:** Single-pass parsing of `regions.csv` populates `region_to_country[10000000]`. By defining `0` as `_UNMATCHED` and countries as `1..num_countries`, the 20 MB static table is zeroed by the OS BSS loader at load time, touching only active pages and eliminating 20 MB `memset` latency.
  3. **Guaranteed CRLF Boundary Chunking:** Exploited spec invariant (CR never appears inside quotes) to split `records.csv` across 16 threads at arbitrary CRLF boundaries with zero false-boundary risk. Handles leading BOM and mid-file BOM at chunk transitions.
  4. **Single-Pass Stream Unescape & FNV-1a Hashing:** Calculates unescaped name byte length and 64-bit FNV-1a hash directly from the input byte stream without intermediate string buffers or memory copying. Notes are unescaped and byte-counted on the fly.
  5. **Contiguous Pre-Allocated Open-Addressing Hash Pool:** Each thread allocates a single 2048-slot open-addressing table pool per country, tracking newly inserted unique entries compactly to accelerate reduction loops.
  6. **Parallel Country Reduction & Lexicographical Format:** Partitions countries across worker threads using precomputed output order (`1..num_countries` alphabetical, followed by `0` `_UNMATCHED`), merges distinct name sets with `DynamicHashSet`, and generates formatted output directly in parallel buffers.
  7. **Integer & Money Formatting LUT:** Unrolled integer and signed two-decimal cents formatting (`format_money`) directly into thread buffers without floating-point math or libc string conversions.
- **Measurements on `data/c_sample` (332,000 records, 22.0 MB dataset):**
  - **Cold Run:** 47.28 ms
  - **Warm Runs (10 iterations):** 38.25 ms, 39.72 ms, 33.45 ms, 38.19 ms, 33.69 ms, 34.21 ms, 34.31 ms, 34.18 ms, 34.21 ms, 34.21 ms
  - **Fastest Wall Time ($T$):** **33.45 ms**
  - **Speedup vs Python Baseline (1,181 ms reference):** **35.3x**
  - **Speedup vs Python Measured (1,366 ms):** **40.8x**
- **Correctness Verification:**
  - `data/c_tiny`: `E4E43BC39920E9D446C3FCD700FDD35B3959E9495C6AAA738491DF38A1AED733` — **EXACT MATCH** (PASS)
  - `data/c_sample`: `9035FED36823232DD41AE2D1B9D6D21AA7C73F49D8E95647E953A89D5EBE9A66` — **EXACT MATCH** (PASS)
  - `WORKED_RECORDS` edge case: **EXACT MATCH** (PASS)
  - `RULES_RECORDS` & variants (leading BOM, final LF, final CRLF): **EXACT MATCH** (PASS)
  - Full test suite `speed-demon-2026/tests/test_c_*.py`: 14 tests — **ALL PASS**
- **Score Impact:**
  - $T_{\text{ref}} / T = 1181 / 33.45 = 35.3 \implies (35.3)^{0.50} \approx 5.94\times$
  - Peak RSS: ~32 MB vs ~185 MB $\implies \min(185/32, 50)^{0.20} \approx (5.78)^{0.20} \approx 1.42\times$
  - Dependencies: $D = 1 \implies 1.0\times$
  - Combined Efficiency Coefficient $K \approx 8.44$, yielding projected points $> 90$.
