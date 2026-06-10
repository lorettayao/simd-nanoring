# HRT Interviewer Critique & Prep Guide

## What's Good (don't undersell these)

1. **Domain choice** — ITCH 5.0 is the actual protocol HRT ingests. Shows you know what matters.
2. **Disruptor-pattern journal** — correct use of release/acquire semantics, cache-line isolation, MESI-aware design. This is directly relevant to how real HFT sequencers work.
3. **Zero-contention sharding** — symbol-hash partitioning means book threads never share mutable state. You can articulate *why* (no CAS, no lock, no false sharing).
4. **Dual-target SIMD** — NEON + AVX2 with scalar fallback shows platform awareness.
5. **Benchmarking infrastructure** — you measured, reported percentiles, and logged per-thread distributions. This is what HRT expects: measure everything.

## What an Interviewer Would Attack

### Critical (fix or have a prepared answer)

| # | Issue | What they'd say | Your answer should be |
|---|-------|-----------------|----------------------|
| 1 | SIMD is slower than scalar | "Your vectorized decode regresses 7%. Why ship it?" | "The workload is gather-dominated. 4 loads at stride-36 to pack one vector. The bswap savings (~3 instructions) don't offset the gather cost. SIMD wins on the ticker filter because the data is already in SoA layout post-decode." |
| 2 | ~~`unordered_map` on hot path~~ (FIXED) | "What's the cache behavior of `live_orders_`?" | "Replaced with open-addressed flat map — all entries inline in one contiguous vector. Linear probing, MurmurHash3 finalizer, ~1 cache miss per lookup. Sharded manager went from 3.53 → 7.63 Mpps (2.16x) just from this change." |
| 3 | ~~O(n) level scan~~ (FIXED) | "256 levels, linear scan on every add/delete?" | "Replaced with sorted arrays + binary search. Bids sorted descending, asks ascending. BBO is always `levels[0]` — O(1). Lookup is O(log n). Sharded manager: 7.63 → 28.07 Mpps." |
| 4 | Every consumer reads all messages | "75% of cache loads are wasted per thread." | "I benchmarked both. Broadcast wins because the producer is the bottleneck: one sequential write stream beats N scattered writes to per-shard queues. Routing adds hash + copy + N atomic updates on the producer's critical path. The consumer waste (reading irrelevant messages) is hidden by the memory subsystem since it's a sequential scan. The right fix is a **hardware NIC-level filter** (RSS/Flow Director) that routes at the network layer before CPU even sees the packet — which is what real HFT does." |
| 5 | ~~No backpressure~~ (FIXED) | "What if producer laps a consumer?" | Now handled — `try_publish()` returns false if it would overwrite unread data. Consumer calls `report_consumer_progress()`. |
| 6 | ~~Latency measurement wrong~~ (FIXED) | "You're measuring wall-clock at publish time, not e2e." | Now fixed — we store publish timestamp, read clock at consume time, diff gives true producer→consumer latency. |

### Medium (be ready to discuss)

| # | Issue | Prepared answer |
|---|-------|----------------|
| 7 | ~~`ShardedBookManager` is 3x slower than single book~~ (FIXED) | "Replaced with open-addressed symbol map. Sharded manager is now *faster* than single book (7.63 vs 6.92 Mpps) because it has better cache locality per-symbol." |
| 8 | ~~No prefetch on decode stream~~ (FIXED) | "`__builtin_prefetch(stream + offset + msg_len)` now prefetches next message while processing current. Hides L1 miss latency on variable-length stream." |
| 9 | ~~Branch misprediction in dispatch~~ (FIXED) | "Replaced switch/case with function pointer table indexed by msg_type byte. One indirect call, no branch chain." |
| 10 | Synthetic data hides real perf | "Real ITCH has bursty arrival, variable message mix, and hot symbols. My uniform random gen doesn't stress worst-case contention or cache pressure." |

## Cache-Friendly Data Structures: What to Replace With

### `unordered_map<uint64_t, LiveOrder>` → Open-addressing flat map

Why unordered_map is bad:
- Node-based: each entry is a separate heap allocation
- Pointer chasing: bucket → node → next node (3+ cache misses)
- No spatial locality: adjacent order_refs are scattered in memory

Better options (best to worst):
1. **Direct-indexed array** `LiveOrder orders[MAX_ORDERS]` indexed by `order_ref % capacity`
   - O(1), zero indirection, perfect cache behavior
   - Works if order_ref space is bounded (it is in ITCH — monotonically increasing)
2. **`absl::flat_hash_map`** or **`robin_hood::unordered_map`**
   - Open-addressing, all entries inline in one contiguous allocation
   - ~1 cache miss per lookup vs. ~3 for std::unordered_map
3. **Swiss table** (what Google/abseil uses internally)
   - SIMD-accelerated probing of metadata bytes

### Price level array → Sorted insertion

Current: unsorted array, linear scan O(n) on every operation.

Better:
- Keep bid levels sorted descending, ask levels sorted ascending
- Binary search for lookup: O(log n) for ~10-20 active levels
- For fixed-tick instruments: direct-indexed array by `(price - min_price) / tick_size`

## What to Build Next (priority order)

1. ~~Replace `unordered_map` with a flat open-addressed map~~ DONE
2. ~~Sorted price levels with binary search~~ DONE
3. ~~Prefetch on decode stream~~ DONE
4. ~~Branchless dispatch table~~ DONE
5. **Add a "SIMD wins" scenario** — decode into SoA layout, then benchmark vectorized downstream filtering. (Ticker filter at 1596 Mpps already demonstrates this.)
6. **Benchmark against naive baseline** — single-threaded decode+book loop. Show the crossover point where pipeline overhead pays off.
7. **Add realistic message mix** — 60% Add, 20% Execute, 15% Delete, 5% Replace matches real NASDAQ traffic ratios.

## One-Liners for the Interview

- "SIMD wins on uniform contiguous data. ITCH structs aren't that — the gather cost dominates."
- "The Disruptor pattern works because consumers never write to the sequence counter's cache line."
- "Symbol-hash sharding gives me zero-contention parallelism — each book thread owns a disjoint set of symbols."
- "The real bottleneck in a book is pointer chasing through the order map, not arithmetic."
- "FPGAs win on deterministic latency and wire-to-logic pipeline, not raw throughput. On the CPU side, the win is data layout."
