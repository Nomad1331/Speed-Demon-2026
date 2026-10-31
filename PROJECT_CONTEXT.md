# Speed Demon 2026 — Master Project Context

## 1. Overview & Competition Reality
- **Competition:** [Speed Demon 2026](https://speeddemon.dev/) (72-hour performance engineering championship).
- **Timeframe:** October 30, 15:00 UTC to November 2, 15:00 UTC.
- **Hardware Platform:** Dedicated bare-metal server in Falkenstein (Helsinki runner). 12 hardware threads (6 P-cores, turbo off), 8 GB RAM (no swap), 512 processes, 1 GB scratch space at `/tmp`.
- **Target:** Beat deliberately slow Python reference implementations across at least 3 tracks to qualify for the **Overall Speed Demon Champion** ($800) and **Highest K** ($200) awards.
- **Primary Rule:** Output must match the expected SHA-256 hash **byte-for-byte** or the track scores zero.

---

## 2. Mathematical Scoring Function & Efficiency Coefficient ($K$)
The official frozen scoring formula (Oct 28 18:00 UTC):

$$K = \left(\frac{T_{\text{ref}}}{T}\right)^{0.50} \times \min\left(\frac{M_{\text{ref}}}{M}, 50\right)^{0.20} \times \min\left(\frac{S_{\text{ref}}}{S}, 1\right)^{0.15} \times \left(\frac{D_{\text{ref}}}{D}\right)^{0.15}$$

$$\text{Points} = \min\left(100, \max\left(0, 50 \times \log_{10}(K)\right)\right)$$

$$\text{Track Total} = 0.65 \times \text{Machine Score} + 0.35 \times \text{Judges Score}$$

$$\text{Overall Title} = \left(\prod_{i=1}^{3} \text{Track}_i\right)^{\frac{1}{3}} \quad \text{(Geometric Mean of top 3 tracks)}$$

### Key Mathematical Insights:
1. **Wall Time ($T$):** Exponent $0.50$, completely **uncapped**! Every order-of-magnitude reduction in runtime directly multiplies $K$.
2. **Memory ($M$):** Exponent $0.20$, capped at $50\times$ improvement. Once peak memory drops to $<2\%$ of reference, memory score is maxed out.
3. **Source Lines ($S$):** Exponent $0.15$, capped at $1.0$. Code can only cost, never pay. If your line count $S \le S_{\text{ref}}$, you get the maximum multiplier ($1.0$). Keep source code concise, modular, and unbloated!
4. **Dependencies ($D$):** $D = 1 + \text{direct non-stdlib deps}$. With **zero external dependencies**, $D = 1$, giving $\left(\frac{1}{1}\right)^{0.15} = 1.0$. Adding third-party packages severely degrades the efficiency coefficient.
5. **Architectural Choice:** Standard modern C++20 / C++23 compiled with `-O3 -march=x86-64-v3 -flto -pthread -static`, zero external dependencies (`dependency_count = 0`), ultra-compact footprint.

---

## 3. The 4 Active Tracks
| Track | Name | Task | Naive Ref $S_{\text{lines}}$ | Output File | Target Runtime |
|---|---|---|---|---|---|
| **A** | **Ingest** | Aggregate 3.1M HTTP log entries into hourly p50/p95/p99 latency, 5xx errors & PPM | 56 lines | `summary.txt` | $< 80\text{ ms}$ |
| **B** | **Index** | 254 MB legal corpus + 50,000 boolean / phrase search queries | 91 lines | `results.txt` | $< 250\text{ ms}$ |
| **C** | **Join** | 200 MB hostile CSV (`records.csv`) joined with `regions.csv` | 108 lines | `aggregates.txt` | $< 120\text{ ms}$ |
| **D** | **Traverse** | 1.74M nodes, 5M directed weighted edges, 2,000 shortest path queries | 43 lines | `distances.txt` | $< 200\text{ ms}$ |

---

## 4. Local Execution & Toolchain Infrastructure
- **Host System:** Windows 10/11 x86_64.
- **Local Toolchain:** Portable 64-bit GCC 14.1.0 (`toolchain/w64devkit/bin/g++.exe`) supporting C++20, C++23, `std::thread`, POSIX threads, AVX2 SIMD.
- **Local Python:** Python 3.12.9 with generators in `speed-demon-2026/generators/`.
- **Remote Production Target:** Linux container built on `gcc:16@sha256:43978d7710a803d1c3d32f4117c1528ec7285a4ce2c0a51e982ec1a6e3d87d95` running on `debian:13-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad`.

---

## 5. Recurrent Thinking & Optimization Protocol
Every performance enhancement cycle operates as a closed feedback loop:
1. **Formulate Hypothesis:** (e.g. Memory mapping vs buffered read, bitmask endpoint hashing vs map).
2. **Implement:** Clean, idiomatic, zero-dependency C++20.
3. **Verify Correctness:** Exact byte-for-byte SHA-256 match against ground truth on `sample` and `full`.
4. **Measure Metrics:** Wall clock time $T$, memory $M$, line count $S$.
5. **Feed Forward:** Record delta in `PERF.md`, update `PROJECT_CONTEXT.md` mental state, and execute next optimization.
