#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <pthread.h>
#include <cstring>
#include "ring_buffer.h" // This includes simd_parser.h and the Itch5AddOrder struct

// Global lock-free ring buffer with a power-of-2 capacity
LockFreeRingBuffer<65536> ring_buffer;

// Atomic flag to cleanly shut down consumer threads when the producer finishes
std::atomic<bool> producer_finished{false};
std::atomic<size_t> total_consumed{0};

// Helper function to pin a thread to a specific physical CPU core
void pin_thread_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

// ---------------------------------------------------------
// CONSUMER: Strategy Thread (Runs on Cores 1, 2, etc.)
// ---------------------------------------------------------
void strategy_thread(int core_id) {
    pin_thread_to_core(core_id);
    
    Itch5AddOrder incoming_trade;
    size_t local_consumed = 0;

    // Spin-wait loop: The thread never sleeps, constantly polling the atomic read index
    while (!producer_finished.load(std::memory_order_relaxed)) {
        if (ring_buffer.pop(incoming_trade)) {
            // In a real system, you would execute your trading algorithm here based on incoming_trade.price
            local_consumed++;
        }
    }

    // Drain any remaining items after the producer signals completion
    while (ring_buffer.pop(incoming_trade)) {
        local_consumed++;
    }

    total_consumed.fetch_add(local_consumed, std::memory_order_relaxed);
}

// ---------------------------------------------------------
// MAIN EXECUTION
// ---------------------------------------------------------
int main() {
    // 1. Pin the main thread (Producer) to Core 0
    pin_thread_to_core(0);
    std::cout << "[System] Producer thread pinned to Core 0.\n";

    // 2. Load the mock data into memory
    const char* filename = "../mock_itch50.bin"; 
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "Fatal Error: Failed to open " << filename << "\n";
        return 1;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    size_t num_messages = size / sizeof(Itch5AddOrder);
    std::vector<Itch5AddOrder> market_data(num_messages);

    std::cout << "[System] Loading " << num_messages << " messages into RAM...\n";
    file.read(reinterpret_cast<char*>(market_data.data()), size);
    file.close();

    // 3. Spawn Consumer Strategy Threads on isolated cores
    const int NUM_CONSUMERS = 2;
    std::vector<std::thread> consumers;
    for (int i = 1; i <= NUM_CONSUMERS; ++i) {
        consumers.emplace_back(strategy_thread, i);
        std::cout << "[System] Spawned Strategy Thread on Core " << i << ".\n";
    }

    // 4. Start the Producer Benchmark
    uint64_t target_ticker;
    std::memcpy(&target_ticker, "AAPL    ", 8);

    std::cout << "\n[Benchmark] Firing data through SIMD Parser to the Ring Buffer...\n";
    auto start_time = std::chrono::high_resolution_clock::now();

    // SIMD Parsing & Pushing Logic
    // (In a fully integrated setup, the push() happens inside the parse_batch loop upon a bitmask match)
    size_t producer_pushed = 0;
    for (size_t i = 0; i < num_messages; ++i) {
        // Simulating the bitmask match for AAPL
        uint64_t current_ticker;
        std::memcpy(&current_ticker, market_data[i].stock, 8);
        
        if (current_ticker == target_ticker) {
            // Spin until the buffer has space to push the payload
            while (!ring_buffer.push(market_data[i])) {
                // Spin-wait (Buffer is full)
            }
            producer_pushed++;
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    
    // 5. Signal completion and join threads
    producer_finished.store(true, std::memory_order_release);
    for (auto& t : consumers) {
        t.join();
    }

    // 6. Output Metrics
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    
    std::cout << "\n--- PERFORMANCE LOG ---\n";
    std::cout << "Total Time       : " << duration_ms << " ms\n";
    std::cout << "Producer Pushed  : " << producer_pushed << " messages\n";
    std::cout << "Consumers Read   : " << total_consumed.load() << " messages\n";
    std::cout << "Data Integrity   : " << (producer_pushed == total_consumed.load() ? "PASSED" : "FAILED") << "\n";
    std::cout << "-----------------------\n";

    return 0;
}