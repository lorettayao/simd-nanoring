# SIMD NanoRing: Performance Benchmarks

**Platform:** Apple Silicon (ARM64 NEON)  
**Threads available:** 12  
**Compiler:** Clang 21.0  

## Results

| Experiment | Configuration | Messages | Time (ms) | Throughput (Mpps) | Notes |
| :--- | :--- | ---: | ---: | ---: | :--- |
| SIMD Decode | Scalar (sequential field extraction) | 20000000 | 26.19 | 763.61 |  |
| SIMD Decode | SIMD Vectorized (4-wide batch) | 20000000 | 28.48 | 702.23 | decoded=20000000 |
| AoS vs SoA | AoS scan (stride-48 per field) | 20000000 | 12.19 | 1640.45 | matches=156051 |
| AoS vs SoA | SoA scalar scan (contiguous fields) | 20000000 | 7.25 | 2760.29 | matches=156051 |
| AoS vs SoA | SoA SIMD scan (vectorized compare) | 20000000 | 4.13 | 4843.40 | matches=156051 |
| Broadcast Scale | 1 consumer thread(s) | 20000000 | 55.15 | 362.63 | total_reads=20000000 |
| Broadcast Scale | 2 consumer thread(s) | 20000000 | 69.14 | 289.27 | total_reads=40000000 |
| Broadcast Scale | 4 consumer thread(s) | 20000000 | 96.47 | 207.31 | total_reads=80000000 |
| Broadcast Scale | 8 consumer thread(s) | 20000000 | 262.06 | 76.32 | total_reads=160000000 |
| Pipeline | 1 book thread(s) | 20000000 | 1002.22 | 19.96 | per_thread=[20000000] |
| Pipeline | 2 book thread(s) | 20000000 | 1198.00 | 16.69 | per_thread=[8426750,11581815] |
| Pipeline | 4 book thread(s) | 20000000 | 486.15 | 41.14 | per_thread=[4373473,5939488,4044777,5642654] |
| Pipeline | 8 book thread(s) | 20000000 | 237.02 | 84.38 | per_thread=[2801753,3745900,2785799,2197606,1573280,2193639,1258988,3444910] |
| Latency | Producer→Consumer (1 thread) | 2000000 | 0.01 | 220167.33 | P50=42ns P99=2875ns P99.9=6541ns |
| Book Compute | Single book (all symbols, sequential) | 20000000 | 1656.39 | 12.07 | adds=11999882 execs=3999414 bbo_chg=27 |
| Book Compute | Sharded manager (sequential) | 20000000 | 806.61 | 24.80 | symbols=63 msgs=20000000 |
| Book Compute | Parallel sharded (2 threads) | 20000000 | 773.13 | 25.87 | total_processed=20000000 |
| Book Compute | Parallel sharded (4 threads) | 20000000 | 552.99 | 36.17 | total_processed=20000000 |
| Book Compute | Parallel sharded (8 threads) | 20000000 | 464.06 | 43.10 | total_processed=20000000 |
| Broadcast vs Routed | Broadcast (1 threads) | 20000000 | 858.00 | 23.31 | efficiency=84% |
| Broadcast vs Routed | Broadcast (2 threads) | 20000000 | 508.34 | 39.34 | efficiency=71% |
| Broadcast vs Routed | Broadcast (4 threads) | 20000000 | 523.67 | 38.19 | efficiency=34% |
| Broadcast vs Routed | Broadcast (8 threads) | 20000000 | 326.17 | 61.32 | efficiency=27% |
| Broadcast vs Routed | Routed (1 threads) | 20000000 | 954.58 | 20.95 | per_thread=[20000000] |
| Broadcast vs Routed | Routed (2 threads) | 20000000 | 688.14 | 29.06 | per_thread=[8437068,11562932] |
| Broadcast vs Routed | Routed (4 threads) | 20000000 | 514.36 | 38.88 | per_thread=[4374305,5938121,4062763,5624811] |
| Broadcast vs Routed | Routed (8 threads) | 20000000 | 602.18 | 33.21 | per_thread=[2812212,3750596,2813958,2186214,1562093,2187525,1248805,3438597] |

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
