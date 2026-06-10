#pragma once
#include "itch_messages.h"
#include "spsc_queue.h"
#include "order_book.h"
#include "pipeline.h"

#include <atomic>
#include <thread>
#include <vector>
#include <chrono>
#include <memory>

// Per-shard SPSC queue: producer routes messages by symbol hash,
// each consumer only reads messages destined for its shard.
// Eliminates the broadcast waste where N-1 out of N cache loads are discarded.
using ShardQueue = SpscQueue<131072>;

class RoutedPipeline {
public:
    struct Config {
        size_t num_book_threads = 4;
        bool measure_latency = true;
    };

    struct Results {
        double total_time_ms = 0;
        double route_time_ms = 0;
        size_t total_messages = 0;
        double throughput_mpps = 0;
        LatencyTracker latency;

        std::vector<size_t> thread_message_counts;
        std::vector<double> thread_throughputs;
    };

    RoutedPipeline(const Config& cfg) : config_(cfg) {}

    Results run(const DecodedOrder* orders, size_t num_orders) {
        Results results;
        results.total_messages = num_orders;
        results.thread_message_counts.resize(config_.num_book_threads, 0);
        results.thread_throughputs.resize(config_.num_book_threads, 0);

        // One SPSC queue per shard — producer writes to the relevant one
        std::vector<std::unique_ptr<ShardQueue>> queues;
        for (size_t i = 0; i < config_.num_book_threads; i++) {
            queues.push_back(std::make_unique<ShardQueue>());
        }

        std::atomic<bool> producer_done{false};
        std::vector<ShardedBookManager> shard_managers(config_.num_book_threads);

        // Per-message publish timestamps for latency
        std::vector<uint64_t> publish_ts;
        if (config_.measure_latency) {
            publish_ts.resize(num_orders, 0);
        }

        auto get_shard = [this](uint64_t ticker_key) -> size_t {
            uint64_t h = ticker_key * 0x9E3779B97F4A7C15ULL;
            h ^= (h >> 33);
            return h % config_.num_book_threads;
        };

        auto start_time = std::chrono::high_resolution_clock::now();

        // --- Consumer threads: each reads ONLY from its own queue ---
        std::vector<std::thread> consumers;
        for (size_t t = 0; t < config_.num_book_threads; t++) {
            consumers.emplace_back([&, t]() {
                pin_to_core(static_cast<int>(t + 1));

                ShardQueue& my_queue = *queues[t];
                size_t local_count = 0;
                DecodedOrder order;

                while (true) {
                    if (my_queue.try_pop(order)) {
                        shard_managers[t].apply(order);
                        local_count++;

                        // Latency sample every 1000th message on thread 0
                        if (config_.measure_latency && t == 0 && (local_count % 1000 == 0)) {
                            auto now = std::chrono::high_resolution_clock::now();
                            uint64_t consume_ns = static_cast<uint64_t>(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    now - start_time).count());
                            // Use a rough estimate — last published message's timestamp
                            uint64_t pub_ns = publish_ts[std::min(local_count * config_.num_book_threads, num_orders - 1)];
                            if (consume_ns > pub_ns) {
                                results.latency.record(consume_ns - pub_ns);
                            }
                        }
                    } else {
                        if (producer_done.load(std::memory_order_acquire) &&
                            my_queue.available() == 0) {
                            break;
                        }
                        cpu_pause();
                    }
                }

                results.thread_message_counts[t] = local_count;
            });
        }

        // --- Producer: batch-route messages to reduce cache-line ping-pong ---
        // Accumulate per-shard, flush when batch is full. This keeps the
        // producer writing to local memory most of the time, only touching
        // the queue's atomic head once per flush (amortized).
        static constexpr size_t BATCH_SIZE = 64;
        std::vector<std::vector<DecodedOrder>> batches(config_.num_book_threads);
        for (auto& b : batches) b.reserve(BATCH_SIZE);

        auto route_start = std::chrono::high_resolution_clock::now();

        for (size_t i = 0; i < num_orders; i++) {
            if (config_.measure_latency) {
                auto now = std::chrono::high_resolution_clock::now();
                publish_ts[i] = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        now - start_time).count());
            }

            size_t shard = get_shard(orders[i].ticker_key);
            batches[shard].push_back(orders[i]);

            if (batches[shard].size() >= BATCH_SIZE) {
                for (auto& msg : batches[shard]) {
                    queues[shard]->push(msg);
                }
                batches[shard].clear();
            }
        }

        // Flush remaining
        for (size_t s = 0; s < config_.num_book_threads; s++) {
            for (auto& msg : batches[s]) {
                queues[s]->push(msg);
            }
        }

        auto route_end = std::chrono::high_resolution_clock::now();
        results.route_time_ms = std::chrono::duration<double, std::milli>(
            route_end - route_start).count();

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
