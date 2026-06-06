#include <iostream>
#include <fstream>
#include <vector>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <random>
#include <thread>

#include "itch_messages.h"
#include "simd_decoder.h"
#include "broadcast_journal.h"
#include "order_book.h"
#include "pipeline.h"

// ============================================================
// DATA GENERATION
// ============================================================

static const char* TICKERS[] = {
    "AAPL    ", "MSFT    ", "TSLA    ", "GOOG    ",
    "AMZN    ", "NFLX    ", "META    ", "NVDA    ",
    "AMD     ", "INTC    ", "JPM     ", "BAC     ",
    "WMT     ", "DIS     ", "ORCL    ", "CRM     ",
    "PYPL    ", "UBER    ", "SQ      ", "SNAP    ",
    "BABA    ", "JD      ", "PDD     ", "NIO     ",
    "PLTR    ", "COIN    ", "HOOD    ", "SOFI    ",
    "RIVN    ", "LCID    ", "F       ", "GM      ",
    "BA      ", "LMT     ", "RTX     ", "NOC     ",
    "XOM     ", "CVX     ", "COP     ", "SLB     ",
    "PFE     ", "JNJ     ", "UNH     ", "ABBV    ",
    "MRK     ", "LLY     ", "TMO     ", "ABT     ",
    "V       ", "MA      ", "AXP     ", "GS      ",
    "MS      ", "C       ", "WFC     ", "USB     ",
    "T       ", "VZ      ", "TMUS    ", "CMCSA   ",
    "NFLX    ", "ROKU    ", "SPOT    ", "ZM      "
};
static constexpr size_t NUM_TICKERS = sizeof(TICKERS) / sizeof(TICKERS[0]);

struct TestData {
    std::vector<DecodedOrder> orders;
    std::vector<ItchAddOrder> raw_add_orders;
};

TestData generate_test_data(size_t num_messages, unsigned seed = 42) {
    TestData data;
    data.orders.resize(num_messages);
    data.raw_add_orders.resize(num_messages);

    std::mt19937 gen(seed);
    std::uniform_int_distribution<size_t> ticker_dist(0, NUM_TICKERS - 1);
    std::uniform_int_distribution<uint32_t> price_dist(100000, 500000);  // $10.00 - $50.00
    std::uniform_int_distribution<uint32_t> shares_dist(1, 1000);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> type_dist(0, 9);

    uint64_t next_order_ref = 1;

    for (size_t i = 0; i < num_messages; i++) {
        size_t ticker_idx = ticker_dist(gen);
        uint64_t ticker_key = 0;
        std::memcpy(&ticker_key, TICKERS[ticker_idx], 8);

        int type_roll = type_dist(gen);

        DecodedOrder& order = data.orders[i];
        order.ticker_key = ticker_key;
        order.price = price_dist(gen);
        order.shares = shares_dist(gen);
        order.side = side_dist(gen) ? 'B' : 'S';
        order.order_ref = next_order_ref++;

        if (type_roll <= 5) {
            order.type = DecodedOrder::Type::Add;
        } else if (type_roll <= 7) {
            order.type = DecodedOrder::Type::Execute;
        } else if (type_roll == 8) {
            order.type = DecodedOrder::Type::Delete;
        } else {
            order.type = DecodedOrder::Type::Replace;
            order.new_order_ref = next_order_ref++;
        }

        // Also generate raw ItchAddOrder for SIMD decode benchmarks
        ItchAddOrder& raw = data.raw_add_orders[i];
        raw.message_type = 'A';
        raw.order_ref_num = order.order_ref;
        raw.buy_sell_indicator = order.side;
        raw.shares = __builtin_bswap32(order.shares);
        std::memcpy(raw.stock, TICKERS[ticker_idx], 8);
        raw.price = __builtin_bswap32(order.price);
    }

    return data;
}

// ============================================================
// BENCHMARK UTILITIES
// ============================================================

struct BenchmarkResult {
    std::string name;
    std::string config;
    size_t messages;
    double time_ms;
    double throughput_mpps;
    std::string extra;
};

std::vector<BenchmarkResult> all_results;

void record_result(const std::string& name, const std::string& config,
                   size_t messages, double time_ms, const std::string& extra = "") {
    double mpps = (messages / 1e6) / (time_ms / 1000.0);
    all_results.push_back({name, config, messages, time_ms, mpps, extra});
    std::cout << "  " << std::left << std::setw(40) << config
              << " | " << std::fixed << std::setprecision(2)
              << std::setw(8) << time_ms << " ms | "
              << std::setw(8) << mpps << " Mpps";
    if (!extra.empty()) std::cout << " | " << extra;
    std::cout << "\n";
}

// ============================================================
// EXPERIMENT 1: SIMD DECODE THROUGHPUT
// Measures raw decoding speed: scalar vs. SIMD vectorized
// ============================================================

void experiment_simd_decode(const TestData& data) {
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "EXPERIMENT 1: SIMD Decode Throughput (Scalar vs. Vectorized)\n";
    std::cout << std::string(70, '=') << "\n";

    size_t n = data.raw_add_orders.size();
    std::vector<DecodedOrder> output(n);

    // Warmup
    SimdDecoder::decode_add_orders_simd(data.raw_add_orders.data(), n, output.data());

    // Scalar decode (force scalar by processing one at a time)
    {
        auto start = std::chrono::high_resolution_clock::now();
        for (size_t i = 0; i < n; i++) {
            output[i].type = DecodedOrder::Type::Add;
            output[i].side = data.raw_add_orders[i].buy_sell_indicator;
            uint64_t tk = 0;
            std::memcpy(&tk, data.raw_add_orders[i].stock, 8);
            output[i].ticker_key = tk;
            uint32_t p;
            std::memcpy(&p, &data.raw_add_orders[i].price, 4);
            output[i].price = __builtin_bswap32(p);
            uint32_t s;
            std::memcpy(&s, &data.raw_add_orders[i].shares, 4);
            output[i].shares = __builtin_bswap32(s);
            std::memcpy(&output[i].order_ref, &data.raw_add_orders[i].order_ref_num, 8);
        }
        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        record_result("SIMD Decode", "Scalar (sequential field extraction)", n, ms);
    }

    // SIMD decode
    {
        auto start = std::chrono::high_resolution_clock::now();
        size_t decoded = SimdDecoder::decode_add_orders_simd(
            data.raw_add_orders.data(), n, output.data());
        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        record_result("SIMD Decode", "SIMD Vectorized (4-wide batch)", n, ms,
                     "decoded=" + std::to_string(decoded));
    }

    // SIMD ticker filter benchmark
    {
        uint64_t target = 0;
        std::memcpy(&target, "AAPL    ", 8);

        auto start = std::chrono::high_resolution_clock::now();
        size_t matches = 0;
        for (size_t i = 0; i + 4 <= n; i += 4) {
            uint32_t mask = SimdDecoder::filter_tickers_simd(output.data() + i, 4, target);
            matches += __builtin_popcount(mask);
        }
        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        record_result("SIMD Decode", "SIMD Ticker Filter (4-wide compare)", n, ms,
                     "matches=" + std::to_string(matches));
    }
}

// ============================================================
// EXPERIMENT 2: BROADCAST JOURNAL SCALING
// Tests read throughput with increasing consumer threads
// ============================================================

void experiment_broadcast_scaling(const TestData& data) {
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "EXPERIMENT 2: Broadcast Journal Consumer Scaling\n";
    std::cout << std::string(70, '=') << "\n";

    size_t n = data.orders.size();
    std::vector<int> thread_counts = {1, 2, 4, 8};

    for (int num_consumers : thread_counts) {
        BroadcastJournal<131072> journal;
        std::atomic<bool> done{false};
        std::vector<size_t> consumer_reads(num_consumers, 0);

        auto start = std::chrono::high_resolution_clock::now();

        // Spawn consumers
        std::vector<std::thread> consumers;
        for (int c = 0; c < num_consumers; c++) {
            consumers.emplace_back([&, c]() {
                pin_to_core(c + 1);
                size_t local_seq = 0;
                while (true) {
                    size_t pub = journal.get_published_index();
                    if (local_seq >= pub) {
                        if (done.load(std::memory_order_acquire)) break;
                        cpu_pause();
                        continue;
                    }
                    while (local_seq < pub) {
                        volatile auto& payload = journal.get_payload<DecodedOrder>(local_seq);
                        (void)payload;
                        local_seq++;
                    }
                }
                consumer_reads[c] = local_seq;
            });
        }

        // Producer
        for (size_t i = 0; i < n; i++) {
            journal.publish(i, data.orders[i]);
        }
        done.store(true, std::memory_order_release);

        for (auto& t : consumers) t.join();

        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        size_t total_read = 0;
        for (auto r : consumer_reads) total_read += r;

        std::string cfg = std::to_string(num_consumers) + " consumer thread(s)";
        record_result("Broadcast Scale", cfg, n, ms,
                     "total_reads=" + std::to_string(total_read));
    }
}

// ============================================================
// EXPERIMENT 3: PIPELINE THROUGHPUT + SCALING
// Full pipeline: decode → journal → sharded book threads
// ============================================================

void experiment_pipeline_scaling(const TestData& data) {
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "EXPERIMENT 3: Pipeline Throughput (Sharded Book Threads)\n";
    std::cout << std::string(70, '=') << "\n";

    size_t n = data.orders.size();
    std::vector<int> thread_counts = {1, 2, 4, 8};

    for (int num_threads : thread_counts) {
        Pipeline::Config cfg;
        cfg.num_book_threads = num_threads;
        cfg.measure_latency = false;

        Pipeline pipeline(cfg);
        auto results = pipeline.run(data.orders.data(), n);

        std::string config_str = std::to_string(num_threads) + " book thread(s)";
        std::string extra = "per_thread=[";
        for (size_t t = 0; t < results.thread_message_counts.size(); t++) {
            if (t > 0) extra += ",";
            extra += std::to_string(results.thread_message_counts[t]);
        }
        extra += "]";

        record_result("Pipeline", config_str, n, results.total_time_ms, extra);
    }
}

// ============================================================
// EXPERIMENT 4: END-TO-END LATENCY DISTRIBUTION
// Measures tick-to-book-update latency at various percentiles
// ============================================================

void experiment_latency(const TestData& data) {
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "EXPERIMENT 4: End-to-End Latency Distribution\n";
    std::cout << std::string(70, '=') << "\n";

    size_t n = std::min(data.orders.size(), size_t(2000000));  // 2M for latency test

    BroadcastJournal<131072> journal;
    std::atomic<bool> done{false};
    std::vector<uint64_t> publish_times(n);
    std::vector<uint64_t> consume_times(n);

    // Single consumer measuring per-message latency
    std::thread consumer([&]() {
        pin_to_core(1);
        size_t local_seq = 0;
        while (true) {
            size_t pub = journal.get_published_index();
            if (local_seq >= pub) {
                if (done.load(std::memory_order_acquire) &&
                    local_seq >= journal.get_published_index()) break;
                cpu_pause();
                continue;
            }
            while (local_seq < pub) {
                auto now = std::chrono::high_resolution_clock::now();
                consume_times[local_seq] = static_cast<uint64_t>(
                    now.time_since_epoch().count());
                volatile auto& payload = journal.get_payload<DecodedOrder>(local_seq);
                (void)payload;
                local_seq++;
            }
        }
    });

    // Producer with timestamps
    for (size_t i = 0; i < n; i++) {
        auto now = std::chrono::high_resolution_clock::now();
        publish_times[i] = static_cast<uint64_t>(now.time_since_epoch().count());
        journal.publish(i, data.orders[i]);
    }
    done.store(true, std::memory_order_release);
    consumer.join();

    // Calculate latency distribution
    std::vector<uint64_t> latencies;
    latencies.reserve(n);
    for (size_t i = 0; i < n; i++) {
        if (consume_times[i] > publish_times[i]) {
            latencies.push_back(consume_times[i] - publish_times[i]);
        }
    }
    std::sort(latencies.begin(), latencies.end());

    if (!latencies.empty()) {
        auto pct = [&](double p) -> uint64_t {
            size_t idx = static_cast<size_t>(p * latencies.size());
            if (idx >= latencies.size()) idx = latencies.size() - 1;
            return latencies[idx];
        };

        std::cout << "  Samples: " << latencies.size() << "\n";
        std::cout << "  P50  : " << pct(0.50) << " ns\n";
        std::cout << "  P90  : " << pct(0.90) << " ns\n";
        std::cout << "  P99  : " << pct(0.99) << " ns\n";
        std::cout << "  P99.9: " << pct(0.999) << " ns\n";
        std::cout << "  Max  : " << latencies.back() << " ns\n";

        // Record to results
        std::string extra = "P50=" + std::to_string(pct(0.50)) + "ns "
                          + "P99=" + std::to_string(pct(0.99)) + "ns "
                          + "P99.9=" + std::to_string(pct(0.999)) + "ns";
        double total_ms = static_cast<double>(latencies.back() - latencies.front()) / 1e6;
        record_result("Latency", "Producer→Consumer (1 thread)", n,
                     total_ms > 0 ? total_ms : 1.0, extra);
    }
}

// ============================================================
// EXPERIMENT 5: ORDER BOOK COMPUTATION THROUGHPUT
// Measures raw book update speed (add/execute/delete/replace)
// ============================================================

void experiment_book_throughput(const TestData& data) {
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "EXPERIMENT 5: Order Book Computation Throughput\n";
    std::cout << std::string(70, '=') << "\n";

    size_t n = data.orders.size();

    // Single-book sequential (baseline)
    {
        OrderBook book;
        auto start = std::chrono::high_resolution_clock::now();
        for (size_t i = 0; i < n; i++) {
            book.apply(data.orders[i]);
        }
        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        std::string extra = "adds=" + std::to_string(book.stats().adds)
                          + " execs=" + std::to_string(book.stats().executes)
                          + " bbo_chg=" + std::to_string(book.stats().bbo_changes);
        record_result("Book Compute", "Single book (all symbols, sequential)", n, ms, extra);
    }

    // Sharded manager sequential
    {
        ShardedBookManager manager;
        auto start = std::chrono::high_resolution_clock::now();
        for (size_t i = 0; i < n; i++) {
            manager.apply(data.orders[i]);
        }
        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        std::string extra = "symbols=" + std::to_string(manager.symbol_count())
                          + " msgs=" + std::to_string(manager.total_messages());
        record_result("Book Compute", "Sharded manager (sequential)", n, ms, extra);
    }

    // Parallel sharded (pre-partition, then parallel apply)
    std::vector<int> shard_counts = {2, 4, 8};
    for (int num_shards : shard_counts) {
        // Pre-partition orders by shard
        std::vector<std::vector<DecodedOrder>> partitions(num_shards);
        for (size_t i = 0; i < n; i++) {
            uint64_t h = data.orders[i].ticker_key * 0x9E3779B97F4A7C15ULL;
            h ^= (h >> 33);
            size_t shard = h % num_shards;
            partitions[shard].push_back(data.orders[i]);
        }

        std::vector<ShardedBookManager> managers(num_shards);

        auto start = std::chrono::high_resolution_clock::now();

        std::vector<std::thread> threads;
        for (int s = 0; s < num_shards; s++) {
            threads.emplace_back([&, s]() {
                pin_to_core(s);
                for (auto& order : partitions[s]) {
                    managers[s].apply(order);
                }
            });
        }
        for (auto& t : threads) t.join();

        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        size_t total_processed = 0;
        for (int s = 0; s < num_shards; s++) {
            total_processed += managers[s].total_messages();
        }

        std::string cfg = "Parallel sharded (" + std::to_string(num_shards) + " threads)";
        std::string extra = "total_processed=" + std::to_string(total_processed);
        record_result("Book Compute", cfg, n, ms, extra);
    }
}

// ============================================================
// RESULTS OUTPUT
// ============================================================

void write_results_markdown() {
    std::ofstream file("../benchmarks.md", std::ios::trunc);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot write benchmarks.md\n";
        return;
    }

    file << "# SIMD NanoRing: Performance Benchmarks\n\n";
    file << "**Platform:** " <<
#if defined(__aarch64__)
        "Apple Silicon (ARM64 NEON)"
#elif defined(__x86_64__)
        "x86-64 (AVX2)"
#else
        "Unknown"
#endif
        << "  \n";
    file << "**Threads available:** " << std::thread::hardware_concurrency() << "  \n";
    file << "**Compiler:** " <<
#if defined(__clang__)
        "Clang " << __clang_major__ << "." << __clang_minor__
#elif defined(__GNUC__)
        "GCC " << __GNUC__ << "." << __GNUC_MINOR__
#else
        "Unknown"
#endif
        << "  \n\n";

    file << "## Results\n\n";
    file << "| Experiment | Configuration | Messages | Time (ms) | Throughput (Mpps) | Notes |\n";
    file << "| :--- | :--- | ---: | ---: | ---: | :--- |\n";

    for (const auto& r : all_results) {
        file << "| " << r.name
             << " | " << r.config
             << " | " << r.messages
             << " | " << std::fixed << std::setprecision(2) << r.time_ms
             << " | " << std::fixed << std::setprecision(2) << r.throughput_mpps
             << " | " << r.extra
             << " |\n";
    }

    file << "\n## Analysis\n\n";
    file << "### Data-Level Parallelism (SIMD)\n";
    file << "The vectorized decoder processes 4 messages per instruction using ";
    file <<
#if defined(__aarch64__)
        "ARM NEON 128-bit"
#else
        "Intel AVX2 256-bit"
#endif
        ;
    file << " registers.\n";
    file << "Byte-swap operations (big-endian ITCH → little-endian host) are vectorized.\n\n";
    file << "### Pipeline Parallelism\n";
    file << "Three-stage pipeline: SIMD Decoder → Broadcast Journal → Sharded Order Books.\n";
    file << "Each stage runs on dedicated cores with zero shared mutable state between book threads.\n\n";
    file << "### Concurrency Scaling\n";
    file << "Symbol-hash sharding eliminates cross-thread contention entirely.\n";
    file << "Broadcast journal keeps index cache line in MESI Shared state (read-only consumers).\n";

    file.close();
    std::cout << "\n[System] Results written to benchmarks.md\n";
}

// ============================================================
// MAIN
// ============================================================

int main() {
    std::cout << "SIMD NanoRing — Experiment Suite\n";
    std::cout << "================================\n";
    std::cout << "Platform: " <<
#if defined(__aarch64__)
        "ARM64 (NEON)"
#elif defined(__x86_64__)
        "x86-64 (AVX2)"
#else
        "Scalar"
#endif
        << "\n";
    std::cout << "Hardware threads: " << std::thread::hardware_concurrency() << "\n";

    const size_t NUM_MESSAGES = 20000000;
    std::cout << "\nGenerating " << NUM_MESSAGES / 1000000 << "M test messages...\n";
    auto data = generate_test_data(NUM_MESSAGES);
    std::cout << "Data generated. Running experiments...\n";

    experiment_simd_decode(data);
    experiment_broadcast_scaling(data);
    experiment_pipeline_scaling(data);
    experiment_latency(data);
    experiment_book_throughput(data);

    write_results_markdown();

    return 0;
}
