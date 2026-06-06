#include <iostream>
#include <vector>
#include <chrono>
#include <cstring>
#include <random>
#include <iomanip>

#include "itch_messages.h"
#include "simd_decoder.h"
#include "pipeline.h"

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
    "ROKU    ", "SPOT    ", "ZM      ", "DOCU    "
};
static constexpr size_t NUM_TICKERS = sizeof(TICKERS) / sizeof(TICKERS[0]);

int main(int argc, char* argv[]) {
    size_t num_messages = 20000000;
    size_t num_threads = 4;

    if (argc > 1) num_threads = std::stoul(argv[1]);
    if (argc > 2) num_messages = std::stoul(argv[2]);

    std::cout << "╔══════════════════════════════════════════════════╗\n";
    std::cout << "║     SIMD NanoRing — Pipeline Market Data Engine  ║\n";
    std::cout << "╚══════════════════════════════════════════════════╝\n\n";

    std::cout << "[Config] Messages: " << num_messages / 1000000 << "M"
              << " | Book Threads: " << num_threads
              << " | Symbols: " << NUM_TICKERS << "\n";
    std::cout << "[Platform] " <<
#if defined(__aarch64__)
        "ARM64 NEON"
#elif defined(__x86_64__)
        "x86-64 AVX2"
#else
        "Scalar"
#endif
        << " | HW Threads: " << std::thread::hardware_concurrency() << "\n\n";

    // --- Stage 0: Generate realistic market data ---
    std::cout << "[Stage 0] Generating " << num_messages / 1000000 << "M order messages...\n";
    std::vector<ItchAddOrder> raw_messages(num_messages);
    std::vector<DecodedOrder> decoded(num_messages);

    std::mt19937 gen(42);
    std::uniform_int_distribution<size_t> ticker_dist(0, NUM_TICKERS - 1);
    std::uniform_int_distribution<uint32_t> price_dist(100000, 500000);
    std::uniform_int_distribution<uint32_t> shares_dist(1, 1000);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> type_dist(0, 9);

    uint64_t next_ref = 1;
    for (size_t i = 0; i < num_messages; i++) {
        size_t tidx = ticker_dist(gen);
        raw_messages[i].message_type = 'A';
        raw_messages[i].order_ref_num = next_ref++;
        raw_messages[i].buy_sell_indicator = side_dist(gen) ? 'B' : 'S';
        raw_messages[i].shares = __builtin_bswap32(shares_dist(gen));
        std::memcpy(raw_messages[i].stock, TICKERS[tidx], 8);
        raw_messages[i].price = __builtin_bswap32(price_dist(gen));
    }

    // --- Stage 1: SIMD Decode ---
    std::cout << "[Stage 1] SIMD Decoding " << num_messages / 1000000 << "M messages...\n";
    auto decode_start = std::chrono::high_resolution_clock::now();

    size_t num_decoded = SimdDecoder::decode_add_orders_simd(
        raw_messages.data(), num_messages, decoded.data());

    // Assign realistic message types for book variety
    for (size_t i = 0; i < num_decoded; i++) {
        int roll = type_dist(gen);
        if (roll <= 5) decoded[i].type = DecodedOrder::Type::Add;
        else if (roll <= 7) decoded[i].type = DecodedOrder::Type::Execute;
        else if (roll == 8) decoded[i].type = DecodedOrder::Type::Delete;
        else {
            decoded[i].type = DecodedOrder::Type::Replace;
            decoded[i].new_order_ref = next_ref++;
        }
    }

    auto decode_end = std::chrono::high_resolution_clock::now();
    double decode_ms = std::chrono::duration<double, std::milli>(decode_end - decode_start).count();
    double decode_mpps = (num_decoded / 1e6) / (decode_ms / 1000.0);

    std::cout << "         Decoded: " << num_decoded << " messages in "
              << std::fixed << std::setprecision(2) << decode_ms << " ms ("
              << decode_mpps << " Mpps)\n\n";

    // --- Stage 2+3: Pipeline (Journal → Sharded Books) ---
    std::cout << "[Stage 2] Broadcasting via zero-copy journal...\n";
    std::cout << "[Stage 3] " << num_threads << " sharded book threads processing...\n\n";

    Pipeline::Config cfg;
    cfg.num_book_threads = num_threads;
    cfg.measure_latency = true;

    Pipeline pipeline(cfg);
    auto results = pipeline.run(decoded.data(), num_decoded);

    // --- Results ---
    std::cout << "┌──────────────────────────────────────────────────┐\n";
    std::cout << "│              PERFORMANCE RESULTS                  │\n";
    std::cout << "├──────────────────────────────────────────────────┤\n";
    std::cout << "│ Total Pipeline Time : " << std::setw(10) << std::fixed
              << std::setprecision(2) << results.total_time_ms << " ms          │\n";
    std::cout << "│ Throughput          : " << std::setw(10) << std::fixed
              << std::setprecision(2) << results.throughput_mpps << " Mpps        │\n";
    std::cout << "│ Decode Stage        : " << std::setw(10) << std::fixed
              << std::setprecision(2) << decode_ms << " ms          │\n";
    std::cout << "├──────────────────────────────────────────────────┤\n";
    std::cout << "│ Per-Thread Message Distribution:                  │\n";

    for (size_t t = 0; t < results.thread_message_counts.size(); t++) {
        std::cout << "│   Thread " << t << ": " << std::setw(10)
                  << results.thread_message_counts[t] << " msgs ("
                  << std::fixed << std::setprecision(2)
                  << results.thread_throughputs[t] << " Mpps)  │\n";
    }

    std::cout << "└──────────────────────────────────────────────────┘\n";

    // Latency (if measured)
    if (results.latency.count > 0) {
        results.latency.sort_samples();
        std::cout << "\n┌──────────────────────────────────────────────────┐\n";
        std::cout << "│              LATENCY PERCENTILES                  │\n";
        std::cout << "├──────────────────────────────────────────────────┤\n";
        std::cout << "│ P50   : " << std::setw(8) << results.latency.percentile(0.50) << " ns                        │\n";
        std::cout << "│ P90   : " << std::setw(8) << results.latency.percentile(0.90) << " ns                        │\n";
        std::cout << "│ P99   : " << std::setw(8) << results.latency.percentile(0.99) << " ns                        │\n";
        std::cout << "│ P99.9 : " << std::setw(8) << results.latency.percentile(0.999) << " ns                        │\n";
        std::cout << "└──────────────────────────────────────────────────┘\n";
    }

    return 0;
}
