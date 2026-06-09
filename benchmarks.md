# SIMD NanoRing: Performance Benchmarks

**Platform:** Apple Silicon (ARM64 NEON)  
**Threads available:** 12  
**Compiler:** Clang 21.0  

## Results

| Experiment | Configuration | Messages | Time (ms) | Throughput (Mpps) | Notes |
| :--- | :--- | ---: | ---: | ---: | :--- |
| SIMD Decode | Scalar (sequential field extraction) | 20000000 | 30.48 | 656.27 |  |
| SIMD Decode | SIMD Vectorized (4-wide batch) | 20000000 | 36.45 | 548.76 | decoded=20000000 |
| SIMD Decode | SIMD Ticker Filter (4-wide compare) | 20000000 | 12.53 | 1596.25 | matches=312242 |
| Broadcast Scale | 1 consumer thread(s) | 20000000 | 53.50 | 373.82 | total_reads=20000000 |
| Broadcast Scale | 2 consumer thread(s) | 20000000 | 70.35 | 284.29 | total_reads=40000000 |
| Broadcast Scale | 4 consumer thread(s) | 20000000 | 102.23 | 195.64 | total_reads=80000000 |
| Broadcast Scale | 8 consumer thread(s) | 20000000 | 256.15 | 78.08 | total_reads=160000000 |
| Pipeline | 1 book thread(s) | 20000000 | 724.09 | 27.62 | per_thread=[20000000] |
| Pipeline | 2 book thread(s) | 20000000 | 467.61 | 42.77 | per_thread=[8426371,11581901] |
| Pipeline | 4 book thread(s) | 20000000 | 367.80 | 54.38 | per_thread=[4372592,5939637,4044791,5642426] |
| Pipeline | 8 book thread(s) | 20000000 | 243.06 | 82.28 | per_thread=[2802465,3745867,2785863,2197628,1573194,2193696,1258956,3444853] |
| Latency | Producer→Consumer (1 thread) | 2000000 | 0.01 | 263713.08 | P50=42ns P99=84ns P99.9=1834ns |
| Book Compute | Single book (all symbols, sequential) | 20000000 | 1582.04 | 12.64 | adds=11999882 execs=3999414 bbo_chg=27 |
| Book Compute | Sharded manager (sequential) | 20000000 | 712.39 | 28.07 | symbols=63 msgs=20000000 |
| Book Compute | Parallel sharded (2 threads) | 20000000 | 647.57 | 30.88 | total_processed=20000000 |
| Book Compute | Parallel sharded (4 threads) | 20000000 | 422.21 | 47.37 | total_processed=20000000 |
| Book Compute | Parallel sharded (8 threads) | 20000000 | 325.94 | 61.36 | total_processed=20000000 |

## Analysis

### Data-Level Parallelism (SIMD)
The vectorized decoder processes 4 messages per instruction using ARM NEON 128-bit registers.
Byte-swap operations (big-endian ITCH → little-endian host) are vectorized.

### Pipeline Parallelism
Three-stage pipeline: SIMD Decoder → Broadcast Journal → Sharded Order Books.
Each stage runs on dedicated cores with zero shared mutable state between book threads.

### Concurrency Scaling
Symbol-hash sharding eliminates cross-thread contention entirely.
Broadcast journal keeps index cache line in MESI Shared state (read-only consumers).
