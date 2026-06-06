# SIMD NanoRing: Performance Benchmarks

**Platform:** Apple Silicon (ARM64 NEON)  
**Threads available:** 12  
**Compiler:** Clang 21.0  

## Results

| Experiment | Configuration | Messages | Time (ms) | Throughput (Mpps) | Notes |
| :--- | :--- | ---: | ---: | ---: | :--- |
| SIMD Decode | Scalar (sequential field extraction) | 20000000 | 25.68 | 778.72 |  |
| SIMD Decode | SIMD Vectorized (4-wide batch) | 20000000 | 27.49 | 727.65 | decoded=20000000 |
| SIMD Decode | SIMD Ticker Filter (4-wide compare) | 20000000 | 13.32 | 1501.51 | matches=312242 |
| Broadcast Scale | 1 consumer thread(s) | 20000000 | 53.78 | 371.86 | total_reads=20000000 |
| Broadcast Scale | 2 consumer thread(s) | 20000000 | 72.41 | 276.19 | total_reads=40000000 |
| Broadcast Scale | 4 consumer thread(s) | 20000000 | 101.91 | 196.26 | total_reads=80000000 |
| Broadcast Scale | 8 consumer thread(s) | 20000000 | 254.44 | 78.61 | total_reads=160000000 |
| Pipeline | 1 book thread(s) | 20000000 | 1819.90 | 10.99 | per_thread=[20000000] |
| Pipeline | 2 book thread(s) | 20000000 | 1164.60 | 17.17 | per_thread=[8418276,11581761] |
| Pipeline | 4 book thread(s) | 20000000 | 653.40 | 30.61 | per_thread=[4373147,5939428,4045284,5642395] |
| Pipeline | 8 book thread(s) | 20000000 | 536.11 | 37.31 | per_thread=[2801303,3745821,2788124,2195718,1571729,2192939,1256700,3444411] |
| Latency | Producer→Consumer (1 thread) | 2000000 | 0.01 | 159987.20 | P50=42ns P99=84ns P99.9=5167ns |
| Book Compute | Single book (all symbols, sequential) | 20000000 | 1715.74 | 11.66 | adds=11999882 execs=3999414 bbo_chg=27 |
| Book Compute | Sharded manager (sequential) | 20000000 | 5669.85 | 3.53 | symbols=63 msgs=20000000 |
| Book Compute | Parallel sharded (2 threads) | 20000000 | 3257.42 | 6.14 | total_processed=20000000 |
| Book Compute | Parallel sharded (4 threads) | 20000000 | 1673.68 | 11.95 | total_processed=20000000 |
| Book Compute | Parallel sharded (8 threads) | 20000000 | 1092.65 | 18.30 | total_processed=20000000 |

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
