#include <iostream>
#include <fstream>
#include <vector>
#include <chrono>
#include <cstring>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <iomanip>
#include "simd_parser.h"
#include "broadcast_journal.h"

// ---------------------------------------------------------
// AUTOMATED LOGGER
// ---------------------------------------------------------
void log_to_markdown(const std::string& experiment, const std::string& impl, size_t messages, double time_ms, double mpps) {
    std::ofstream file("../benchmarks.md", std::ios::app); // Append mode
    if (file.is_open()) {
        file << "| " << experiment << " | " << impl << " | " << messages << " | " 
             << std::fixed << std::setprecision(2) << time_ms << " | " 
             << std::fixed << std::setprecision(2) << mpps << " |\n";
        file.close();
    }
}

void init_markdown_log() {
    std::ofstream file("../benchmarks.md", std::ios::trunc); // Overwrite mode
    if (file.is_open()) {
        file << "# SIMD NanoRing: Performance Benchmarks\n\n";
        file << "| Experiment | Implementation | Total Messages | Total Time (ms) | Throughput (Mpps) |\n";
        file << "| :--- | :--- | :--- | :--- | :--- |\n";
        file.close();
    }
}

// ---------------------------------------------------------
// EXPERIMENT C: ZERO-COPY BROADCAST SCALABILITY
// ---------------------------------------------------------
void run_experiment_c(const std::vector<Itch5AddOrder>& data) {
    std::cout << "\n--- EXPERIMENT C: Zero-Copy Broadcast Scalability ---\n";
    size_t num_items = data.size();
    
    // We will test 1, 2, and 4 consumers to prove linear scaling without MESI cache storms
    std::vector<int> thread_counts = {1, 2, 4};

    for (int num_consumers : thread_counts) {
        BroadcastJournal<65536> journal;
        std::atomic<size_t> total_consumer_reads{0};
        
        auto start_time = std::chrono::high_resolution_clock::now();

        // 1. Spawn Consumers
        std::vector<std::thread> consumers;
        for (int i = 0; i < num_consumers; ++i) {
            consumers.emplace_back([&journal, num_items, &total_consumer_reads]() {
                size_t local_sequence = 0;
                size_t reads = 0;
                
                while (local_sequence < num_items) {
                    // Spin-wait for the producer to publish the next index
                    while (journal.get_published_index() <= local_sequence) {
                        // In production, we use the _mm_pause() intrinsic here to rest the CPU
                    }
                    
                    // Retrieve payload in O(1)
                    volatile auto payload = journal.get_payload(local_sequence);
                    (void)payload; // Prevent compiler from optimizing away the read
                    
                    local_sequence++;
                    reads++;
                }
                total_consumer_reads.fetch_add(reads, std::memory_order_relaxed);
            });
        }

        // 2. Run Producer
        for (size_t i = 0; i < num_items; ++i) {
            journal.publish(i, data[i]);
        }

        // 3. Join threads and measure
        for (auto& t : consumers) { t.join(); }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
        double mpps = (num_items / 1000000.0) / (ms / 1000.0);

        std::cout << "Broadcast Journal (" << num_consumers << " Consumers) Time : " << ms << " ms | " << mpps << " Mpps\n";
        
        log_to_markdown("3. Scaling", "Broadcast (" + std::to_string(num_consumers) + " threads)", num_items, ms, mpps);
    }
}

// ---------------------------------------------------------
// MAIN
// ---------------------------------------------------------
int main() {
    init_markdown_log(); // Create fresh log file
    
    std::cout << "Generating 10,000,000 mock messages in memory...\n";
    size_t total_messages = 10000000;
    std::vector<Itch5AddOrder> data(total_messages);
    
    // Quick populate
    for(size_t i = 0; i < total_messages; ++i) {
        std::memcpy(data[i].stock, "AAPL    ", 8); 
    }

    // Execute the new Broadcast scaling experiment
    run_experiment_c(data); 

    std::cout << "\n[System] Benchmarks successfully written to benchmarks.md\n";
    return 0;
}