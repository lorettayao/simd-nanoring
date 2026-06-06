#pragma once
#include "itch_messages.h"
#include "simd_decoder.h"
#include "broadcast_journal.h"
#include "order_book.h"

#include <atomic>
#include <thread>
#include <vector>
#include <chrono>
#include <cstring>
#include <functional>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

// Platform-agnostic spin-wait hint
static inline void cpu_pause() {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
    __asm__ volatile("yield" ::: "memory");
#endif
}

// Platform-agnostic core pinning (Linux only; no-op on macOS)
static inline void pin_to_core([[maybe_unused]] int core_id) {
#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
}

// Latency measurement via monotonic high-resolution clock
struct LatencyTracker {
    static constexpr size_t MAX_SAMPLES = 1000000;

    std::vector<uint64_t> samples;
    size_t count = 0;

    LatencyTracker() { samples.resize(MAX_SAMPLES); }

    void record(uint64_t ns) {
        if (count < MAX_SAMPLES) {
            samples[count++] = ns;
        }
    }

    void sort_samples() {
        std::sort(samples.begin(), samples.begin() + count);
    }

    uint64_t percentile(double p) const {
        if (count == 0) return 0;
        size_t idx = static_cast<size_t>(p * count);
        if (idx >= count) idx = count - 1;
        return samples[idx];
    }
};

// The broadcast journal carrying decoded orders between pipeline stages
using OrderJournal = BroadcastJournal<131072>;  // 128K slot ring

// Full pipeline: Decoder → Journal → N Sharded Book Threads
class Pipeline {
public:
    struct Config {
        size_t num_book_threads = 4;
        size_t num_symbols = 64;       // total symbols in universe
        size_t decode_batch_size = 256; // messages decoded before publishing
        bool measure_latency = true;
    };

    struct Results {
        double total_time_ms = 0;
        double decode_time_ms = 0;
        size_t total_messages = 0;
        size_t messages_per_thread = 0;
        double throughput_mpps = 0;
        LatencyTracker latency;

        // Per-thread stats
        std::vector<size_t> thread_message_counts;
        std::vector<double> thread_throughputs;
    };

    Pipeline(const Config& cfg) : config_(cfg) {}

    // Run the full pipeline on pre-decoded orders (for controlled benchmarking)
    Results run(const DecodedOrder* orders, size_t num_orders) {
        Results results;
        results.total_messages = num_orders;
        results.thread_message_counts.resize(config_.num_book_threads, 0);
        results.thread_throughputs.resize(config_.num_book_threads, 0);

        OrderJournal journal;
        std::atomic<bool> producer_done{false};
        std::vector<ShardedBookManager> shard_managers(config_.num_book_threads);
        std::vector<std::thread> consumers;

        // Assign symbol shards: symbol hash % num_threads
        auto get_shard = [this](uint64_t ticker_key) -> size_t {
            // FNV-1a inspired hash for uniform distribution
            uint64_t h = ticker_key * 0x9E3779B97F4A7C15ULL;
            h ^= (h >> 33);
            return h % config_.num_book_threads;
        };

        // --- STAGE 2: Book Builder Threads ---
        auto start_time = std::chrono::high_resolution_clock::now();

        for (size_t t = 0; t < config_.num_book_threads; t++) {
            consumers.emplace_back([&, t]() {
                pin_to_core(static_cast<int>(t + 1));

                size_t local_seq = 0;
                size_t local_count = 0;

                while (true) {
                    size_t published = journal.get_published_index();

                    if (local_seq >= published) {
                        if (producer_done.load(std::memory_order_acquire) &&
                            local_seq >= journal.get_published_index()) {
                            break;
                        }
                        cpu_pause();
                        continue;
                    }

                    // Drain all available messages
                    while (local_seq < published) {
                        const DecodedOrder& order = journal.get_payload<DecodedOrder>(local_seq);

                        // Only process orders belonging to this shard
                        if (get_shard(order.ticker_key) == t) {
                            shard_managers[t].apply(order);
                            local_count++;
                        }
                        local_seq++;
                    }
                }

                results.thread_message_counts[t] = local_count;
            });
        }

        // --- STAGE 1: Producer (Decoder → Journal) ---
        auto decode_start = std::chrono::high_resolution_clock::now();

        for (size_t i = 0; i < num_orders; i++) {
            journal.publish(i, orders[i]);

            // Optional latency sampling every 1000th message
            if (config_.measure_latency && (i % 1000 == 0)) {
                auto now = std::chrono::high_resolution_clock::now();
                auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - start_time).count();
                results.latency.record(static_cast<uint64_t>(ns));
            }
        }

        auto decode_end = std::chrono::high_resolution_clock::now();
        results.decode_time_ms = std::chrono::duration<double, std::milli>(
            decode_end - decode_start).count();

        producer_done.store(true, std::memory_order_release);

        // --- Join ---
        for (auto& t : consumers) { t.join(); }

        auto end_time = std::chrono::high_resolution_clock::now();
        results.total_time_ms = std::chrono::duration<double, std::milli>(
            end_time - start_time).count();
        results.throughput_mpps = (num_orders / 1e6) / (results.total_time_ms / 1000.0);

        for (size_t t = 0; t < config_.num_book_threads; t++) {
            results.thread_throughputs[t] = (results.thread_message_counts[t] / 1e6) /
                                            (results.total_time_ms / 1000.0);
        }

        return results;
    }

private:
    Config config_;
};
