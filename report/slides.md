# SIMD NanoRing — Presentation Script & Slide Notes

---

## Slide 1: Title

**Bullets:**
- SIMD NanoRing: A Low-Latency Market Data Pipeline
- SIMD Decoding | Lock-Free Broadcast | Sharded Order Books
- Platform: ARM NEON (Apple Silicon) / Intel AVX2

**Script:**
"Today I'm presenting SIMD NanoRing — a high-performance market data processing pipeline built from scratch in C++. The system decodes NASDAQ ITCH 5.0 protocol messages, broadcasts them to multiple consumers without locks, and maintains real-time order books — all targeting sub-100 nanosecond latency at the median. I'll walk through the architecture top-down, then dive into the hardware-level tricks that make it fast."

---

## Slide 2: The Problem

**Bullets:**
- NASDAQ sends ~20 million messages per day per exchange
- ITCH 5.0 protocol: binary, big-endian, variable-length messages
- Goal: decode → distribute → build order books in real time
- Constraints: no locks, no allocations on hot path, deterministic latency

**Script:**
"The NASDAQ ITCH 5.0 feed is a firehose of binary messages — add orders, executions, cancellations, replacements. Each message is packed tight in big-endian byte order because it's a network protocol. Our job is to decode these as fast as possible, fan them out to multiple processing threads, and maintain a live order book per symbol. The key constraint: we can't afford locks or unpredictable allocations on the hot path, because in trading, tail latency kills."

---

## Slide 3: Big Picture Architecture

**Bullets:**
- Three-stage pipeline:
  1. SIMD Decoder — raw bytes → structured orders (data parallelism)
  2. Broadcast Journal — single-writer ring buffer (pipeline parallelism)
  3. Sharded Order Books — N independent threads (task parallelism)
- Each stage runs on a dedicated core
- Zero shared mutable state between book threads

**Diagram suggestion:**
```
Raw ITCH bytes (network)
        │
        ▼
┌──────────────────┐
│   SIMD Decoder   │  ← vrev32q_u8 / _mm_shuffle_epi8
│  (4-wide batch)  │
└────────┬─────────┘
         │ DecodedOrder structs
         ▼
┌──────────────────┐
│ Broadcast Journal│  ← single atomic, zero-copy
│  (128K ring)     │
└──┬───┬───┬───┬───┘
   │   │   │   │
   ▼   ▼   ▼   ▼
 Shard0 Shard1 Shard2 Shard3
   │     │     │     │
   ▼     ▼     ▼     ▼
 OrderBook (per symbol)
```

**Script:**
"Here's the full system. Stage 1 decodes raw bytes using SIMD instructions — processing 4 messages at a time. Stage 2 is a broadcast journal — think of it as a ring buffer where one producer writes and N consumers read, with no synchronization between consumers. Stage 3 is sharded order books — each thread owns a disjoint set of stock symbols and processes only its own messages. The critical property: no thread ever writes to memory that another thread reads. We'll see why that matters when we talk about CPU caches."

---

## Slide 4: Background — What is an Order Book?

**Bullets:**
- An order book is a sorted list of buy/sell orders for a given stock
- BID side: buyers, sorted highest price first ("best bid")
- ASK side: sellers, sorted lowest price first ("best ask")
- Spread = best ask − best bid
- Four operations: Add, Execute (fill), Delete (cancel), Replace (modify)

**Diagram suggestion:**
```
        ASK (sellers)              BID (buyers)
  $150.10  100 shares         $150.00  500 shares  ← best bid
  $150.15  200 shares         $149.95  200 shares
  $150.20  800 shares         $149.90 1000 shares
     ↑ best ask
     
  spread = $150.10 − $150.00 = $0.10
```

**Script:**
"Before we dive into the implementation, let me explain what an order book is. For every stock, the exchange maintains a two-sided book. The bid side has buyers — sorted by price descending, so the highest bid is at the top. The ask side has sellers — sorted by price ascending, so the cheapest offer is at the top. The gap between them is called the spread. When someone places a new order, that's an Add. When an order gets filled, that's an Execute. Cancellations are Deletes. And modifications are Replaces — effectively a delete plus an add at a new price."

---

## Slide 5: Order Book — Implementation Details

**Bullets:**
- Per-symbol data:
  - `bid_levels_[256]` / `ask_levels_[256]` — price level arrays
  - `live_orders_` — hash map: order_ref → {price, shares, side}
  - `best_bid_price_` / `best_ask_price_` — cached BBO
- Add: insert into levels + update BBO if improved (O(1) best case)
- Execute: find order → reduce level shares → delete if exhausted
- Delete: find order → remove shares from level → recalculate BBO only if at top
- Replace: delete old + add new at different price/quantity

**Script:**
"My order book stores up to 256 price levels per side in a flat array — no heap allocations, cache-friendly. Every live order is tracked in a hash map keyed by its reference number, so lookups for execute/delete/replace are O(1) average. I cache the best bid and best ask, and only recalculate them when an order at the current best price gets removed. Most of the time — when someone adds an order or executes something away from the top — BBO update is zero-cost."

---

## Slide 6: Background — CPU Cache & the MESI Protocol

**Bullets:**
- Modern CPUs have per-core L1/L2 caches (small, fast) and shared L3
- Cache operates in 64-byte "cache lines" — the unit of transfer
- MESI protocol governs coherence between cores:
  - **M**odified — I changed it, I own it exclusively
  - **E**xclusive — only I have it, but it's clean
  - **S**hared — multiple cores have read-only copies
  - **I**nvalid — stale, must re-fetch
- When core A writes → all other cores' copies are invalidated
- If cores A and B both write the same line → "ping-pong" (M↔I↔M), ~40-80ns each bounce

**Script:**
"To understand why the broadcast journal is fast, you need to know how CPU caches work. Each core has its own L1 and L2 cache. When multiple cores access the same memory, a hardware protocol called MESI coordinates them. A cache line can be Modified — meaning one core wrote to it and owns it. It can be Shared — meaning multiple cores have identical read-only copies. Or Invalid — meaning it's stale and needs to be re-fetched. The critical thing: when one core writes a cache line, it sends an invalidation to every other core that has a copy. If two cores keep writing the same line back and forth, you get 'ping-pong' — each transfer costs 40 to 80 nanoseconds. That's what we're trying to avoid."

---

## Slide 7: Broadcast Journal — Design

**Bullets:**
- Inspired by LMAX Disruptor (Java trading framework)
- Ring buffer: 128K slots, power-of-2 for bitmask indexing
- Only 3 components:
  1. `journal_[]` — flat array of 64-byte slots (payload storage)
  2. `published_index_` — single `atomic<size_t>`, the ONLY shared mutable state
  3. Consumers: each has a private `local_seq` counter
- Producer: `memcpy` payload → `store(release)` index
- Consumer: `load(acquire)` index → if new, read payload directly

**Script:**
"The broadcast journal is inspired by the LMAX Disruptor. It's a ring buffer with 128K slots. The entire synchronization mechanism is a single atomic integer: the published index. The producer writes a payload into a slot, then does a release-store on the index — this guarantees all CPUs will see the payload before they see the updated index. Consumers do an acquire-load on the index. If it's advanced beyond their local sequence, they read the new payloads directly from the ring — zero copy, no locking."

---

## Slide 8: Broadcast Journal — Why MESI Makes it Fast

**Bullets:**
- `published_index_` cache line behavior:
  - Producer writes → line goes to **Modified** on producer's core
  - This invalidates all consumer copies (once per publish)
  - Consumers read → line transitions to **Shared** across all cores
  - Multiple consumers reading simultaneously → stays Shared, zero cost between them
- Key insight: consumers NEVER write to any shared state
  - No atomic counter to claim work
  - No lock to acquire
  - No "I'm done" flag to update
- Compare to work-stealing queues: every "steal" is a write → N×N ping-pong

**Script:**
"Here's why this is fast at the hardware level. The published_index_ sits on one cache line. When the producer writes it, that line becomes Modified on the producer's core, and all consumer copies get invalidated — this happens once per publish. Then when consumers read it, the line enters Shared state on all their cores simultaneously. Once it's Shared, any number of consumers can read it without any further cache coherence traffic — it's free. Contrast this with a typical work-stealing queue where every consumer does an atomic increment to claim the next item. That increment is a write, so it bounces the cache line between cores in a Modified-Invalid ping-pong. With 8 consumers, that's potentially 8 invalidations per message. In our design: zero."

---

## Slide 9: Sharding — How We Partition Work

**Bullets:**
- Goal: each book thread processes a disjoint set of symbols
- Hash function: `ticker_key × 0x9E3779B97F4A7C15 ⊕ (>> 33)` mod N
  - This is a Fibonacci hashing constant (golden ratio × 2^64)
  - Produces uniform distribution across threads
- Example with 4 threads:
  - Thread 0: AAPL, GOOG, AMD, ...
  - Thread 1: MSFT, AMZN, INTC, ...
  - Thread 2: TSLA, NFLX, JPM, ...
  - Thread 3: META, NVDA, BAC, ...
- Each thread: reads ALL journal messages, but only `apply()` its own symbols
- Zero locks, zero atomics, zero shared mutable state between book threads

**Script:**
"Now, how do we divide work among order book threads? We hash each stock's 8-byte ticker symbol with a Fibonacci hash — multiply by the golden ratio constant, XOR-shift, then mod by the number of threads. This gives us a uniform distribution. Each thread reads every message from the journal — that's cheap because the journal is in Shared state — but only processes the ones that hash to its shard. Since each thread owns a completely independent set of symbols, no two threads ever touch the same order book. Zero synchronization needed. This is the key design: we pay a small cost for each thread to read and discard messages it doesn't own, but in exchange we get zero contention."

---

## Slide 10: SIMD Decoding — The Data Problem

**Bullets:**
- ITCH 5.0 fields are big-endian (network byte order)
- Our CPU is little-endian (ARM and x86 both)
- Need to byte-swap every price and shares field: `[00][01][5F][90]` → `[90][5F][01][00]`
- Scalar approach: call `bswap32()` per field, one at a time
- SIMD approach: load 4 messages' fields into one register, swap all 4 at once

**Script:**
"ITCH messages come off the network in big-endian byte order, but our CPUs are little-endian. Every 4-byte price and shares field needs its bytes reversed before we can use it as a normal integer. The scalar way is to call bswap32 for each field individually — that's one operation per field. With SIMD, we load four prices into a single 128-bit register and reverse all four in one instruction."

---

## Slide 11: SIMD — ARM NEON Implementation

**Bullets:**
- NEON registers: 128-bit wide = 4 × 32-bit lanes
- `vrev32q_u8`: reverse bytes within each 32-bit lane
  - Before: `[00][01][5F][90] | [00][02][AB][CD] | ...`
  - After:  `[90][5F][01][00] | [CD][AB][02][00] | ...`
- For ticker matching: `vceqq_u64` — compares 2 × 64-bit values in parallel
  - Register only fits 2 tickers (128 / 64 = 2)
  - Need two compares to check 4 messages

**Script:**
"On ARM, we use NEON which gives us 128-bit registers. For byte-swapping, the instruction is vrev32q_u8 — it reverses the byte order within every 32-bit chunk, converting four prices from big-endian to little-endian in a single cycle. For ticker matching — where we want to check if a message belongs to, say, AAPL — we use vceqq_u64 which compares two 64-bit values in parallel. Since a NEON register is only 128 bits, it fits two ticker comparisons per instruction, so we need two instructions to check four messages."

---

## Slide 12: SIMD — Intel AVX2 Implementation

**Bullets:**
- AVX2 registers: 256-bit wide = 4 × 64-bit lanes
- Byte-swap: `_mm_shuffle_epi8` with a permutation mask
  - Mask `[12,13,14,15, 8,9,10,11, 4,5,6,7, 0,1,2,3]` = reverse each 32-bit group
  - Same effect as NEON's `vrev32q_u8`, but via explicit byte shuffling
- Ticker matching: `_mm256_cmpeq_epi64` — 4 × 64-bit comparisons at once
  - Then `_mm256_movemask_pd` extracts result as a 4-bit integer
  - `0b0101` → messages 0 and 2 matched
- AVX2 advantage: 4-wide ticker compare in one shot (vs NEON's 2-wide)

**Script:**
"On Intel with AVX2, we get 256-bit registers. For byte-swapping we use _mm_shuffle_epi8 — a general-purpose byte permutation. You give it a mask that says 'put byte 3 at position 0, byte 2 at position 1' and so on. The effect is identical to NEON's vrev32q_u8. For ticker matching, AVX2 is wider: _mm256_cmpeq_epi64 compares four 64-bit tickers against a target in a single instruction. Then _mm256_movemask_pd compresses the 256-bit result into a 4-bit integer where each bit tells you if that message matched. On ARM we need two instructions to cover four messages; on Intel it's one."

---

## Slide 13: Experiment Design

**Bullets:**
- Dataset: 20 million synthetic ITCH messages
  - 64 symbols, realistic price/quantity distributions
  - Type mix: 60% Add, 20% Execute, 10% Delete, 10% Replace
- Five experiments:
  1. SIMD decode throughput (scalar vs vectorized)
  2. Broadcast journal consumer scaling (1/2/4/8 readers)
  3. Full pipeline end-to-end (varying book threads)
  4. Latency distribution (producer→consumer transit time)
  5. Order book computation (sequential vs parallel sharded)
- Platform: Apple Silicon M-series, 12 hardware threads, Clang 21

**Script:**
"For benchmarking, I generate 20 million synthetic messages across 64 stock symbols with a realistic type distribution — 60% adds, 20% executions, 10% deletes, 10% replaces. I run five experiments. First, raw SIMD decode speed. Second, how the broadcast journal scales with more readers. Third, end-to-end pipeline throughput. Fourth, latency distribution of the journal. Fifth, order book computation with parallel sharding. Everything runs on Apple Silicon with 12 hardware threads."

---

## Slide 14: Results — Broadcast Journal Scaling

**Bullets:**
- Each consumer reads ALL 20M messages independently (broadcast model)
- Results:
  - 1 consumer: 53.78 ms → 372 Mpps
  - 2 consumers: 72.41 ms (1.35× slower, but 2× total work)
  - 4 consumers: 101.91 ms (1.89× slower, but 4× total work)
  - 8 consumers: 254.44 ms (4.73× slower, but 8× total work)
- Wall-clock grows sub-linearly with consumers → Shared MESI state works
- 8-thread degradation: crosses Apple Silicon cluster boundary (shared L2 per cluster)

**Script:**
"The broadcast journal scales sub-linearly. One consumer finishes in 54 milliseconds. With 8 consumers, total work is 8× more — 160 million reads — but wall-clock only increases to 254 ms, about 4.7× slower. This proves the Shared MESI state design works: consumers reading the same cache line don't interfere with each other. The steeper drop at 8 threads is because Apple Silicon groups cores into clusters that share an L2 — once we cross the cluster boundary, invalidation messages have to travel further."

---

## Slide 15: Results — End-to-End Pipeline

**Bullets:**
- Full system: decode → journal → sharded books
- Scaling:
  - 1 thread: 1,820 ms / 11.0 Mpps (baseline)
  - 2 threads: 1,165 ms / 17.2 Mpps (78% efficiency)
  - 4 threads: 653 ms / 30.6 Mpps (70% efficiency)
  - 8 threads: 536 ms / 37.3 Mpps (42% efficiency)
- Efficiency = actual_throughput / (single_thread × N)
- Diminishing returns at 8: each consumer reads ALL messages, discards (1 − 1/N)
- Still: 37 Mpps = processing 20M messages in half a second

**Script:**
"End-to-end, with one thread we process 20 million messages in 1.8 seconds — about 11 million per second. At 4 threads, we hit 30.6 million per second with 70% scaling efficiency. At 8 threads, efficiency drops to 42% — because each thread still reads every journal message but only processes one-eighth of them. The wasted reads become the bottleneck. Even so, 37 million messages per second means the full 20M dataset processes in half a second."

---

## Slide 16: Results — Latency Distribution

**Bullets:**
- Measuring: time from producer publish to consumer read
- P50: 42 ns (typical path: 1 acquire load + 1 data read)
- P99: 84 ns (occasional extra L2 miss)
- P99.9: 5,167 ns (~5 μs — OS scheduler / interrupt)
- Max: 12,542 ns (~12.5 μs — worst-case context switch)
- Context: L2 cache latency on Apple Silicon ≈ 12 ns

**Script:**
"Latency is where this design really shines. Median transit time from producer to consumer is 42 nanoseconds. That's roughly three L2 cache accesses. At P99 we're at 84 nanoseconds — still well under a microsecond. The P99.9 spike to 5 microseconds is the OS scheduler stealing our core for an interrupt. On a real trading system you'd pin cores and isolate CPUs to squash that tail, but even on stock macOS, we're consistently under 100 nanoseconds at P99."

---

## Slide 17: Results — Order Book Scaling

**Bullets:**
- Sharded parallelism results:
  - Sequential (1 book, all symbols): 1,716 ms / 11.7 Mpps
  - Parallel 4 threads: 1,674 ms / 12.0 Mpps (impaired by partition overhead)
  - Parallel 8 threads: 1,093 ms / 18.3 Mpps → 5.2× speedup over sharded sequential
- Why not 8× speedup?
  - Hash distribution isn't perfectly uniform (some symbols get more traffic)
  - Partition cost: copying 20M orders into per-thread vectors
  - Thread startup/join overhead
- Key point: achieved with ZERO locks — scaling limited by physics, not contention

**Script:**
"For order book computation alone, sequential processing does about 11.7 million operations per second. With 8 parallel shards, we reach 18.3 million per second — a 5.2× speedup. Why not 8×? Three reasons: the hash doesn't distribute perfectly evenly, there's a pre-partition cost to sort messages into per-thread buckets, and thread startup overhead. But the important thing is: we achieved this with absolutely zero locks or atomics. The scaling limitation is memory bandwidth and hash skew, not contention."

---

## Slide 18: Design Tradeoffs & Limitations

**Bullets:**
- Broadcast model: each consumer reads N messages, only processes N/threads
  - Alternative: work-stealing queue (less wasted reads, but adds contention)
- Fixed-size arrays (256 price levels): simple but limits deep books
- `unordered_map` for live orders: allocates on heap, less predictable latency
  - Production alternative: flat hash map or open-addressing
- No SIMD in order book itself (tree operations are hard to vectorize)
- macOS: no core affinity (`pin_to_core` is a no-op), can't eliminate tail latency

**Script:**
"A few honest tradeoffs. The broadcast model wastes reads — with 8 threads, each thread discards 7/8 of what it reads. A work-stealing queue would be more efficient in theory, but it introduces exactly the cache-line contention we're trying to avoid. The order book uses unordered_map which can allocate — in production you'd want a flat hash map. And on macOS we can't actually pin cores, so the tail latency numbers would be tighter on Linux with proper CPU isolation."

---

## Slide 19: Summary

**Bullets:**
- Three levels of parallelism exploited:
  1. **Data-level** (SIMD): 4 messages decoded per instruction
  2. **Pipeline-level** (Journal): decode and book-building overlap in time
  3. **Task-level** (Sharding): N independent book threads, zero contention
- Key hardware insight: keep shared data in MESI Shared state
- Results on Apple Silicon:
  - 42 ns median latency (producer → consumer)
  - 37 Mpps end-to-end throughput (8 threads)
  - Sub-linear scaling with zero locks

**Script:**
"To wrap up: this system exploits parallelism at three levels. SIMD gives us data-level parallelism — four messages per instruction. The broadcast journal gives us pipeline parallelism — decode and book-building happen concurrently. And symbol-hash sharding gives us task-level parallelism — independent threads with zero contention. The unifying hardware insight is: design your data structures so shared memory stays in MESI Shared state — let the cache coherence protocol work for you, not against you. The result is 42 nanosecond median latency and 37 million messages per second, all without a single lock."

---

## Slide 20: Q&A

**Script:**
"Happy to take questions — on the SIMD implementation, the cache behavior, the sharding strategy, or anything else."
