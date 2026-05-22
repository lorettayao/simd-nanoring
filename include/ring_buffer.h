#pragma once
#include <atomic>
#include <vector>
#include "simd_parser.h" // We need the Itch5AddOrder struct

// We use a template so the capacity is known at compile time.
// Capacity MUST be a power of 2 (e.g., 1024, 2048, 65536) for ultra-fast bitwise masking.
template <size_t Capacity>
class LockFreeRingBuffer {
private:
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    static constexpr size_t Mask = Capacity - 1;

    std::vector<Itch5AddOrder> buffer;

    // CRITICAL HFT OPTIMIZATION: False Sharing Elimination
    // If the producer and consumer indices share the same 64-byte L1 cache line, 
    // they will constantly invalidate each other across cores. 
    // alignas(64) forces physical separation in the CPU cache.
    alignas(64) std::atomic<size_t> write_index{0};
    alignas(64) std::atomic<size_t> read_index{0};

public:
    LockFreeRingBuffer() : buffer(Capacity) {}

    // ---------------------------------------------------------
    // PRODUCER (Core 0 - The AVX2 Bouncer)
    // ---------------------------------------------------------
    bool push(const Itch5AddOrder& item) {
        // Relaxed load because only the single producer ever changes the write_index
        size_t current_write = write_index.load(std::memory_order_relaxed);
        
        // Acquire load to ensure we see the most up-to-date read_index from consumers
        size_t current_read = read_index.load(std::memory_order_acquire);

        if (current_write - current_read >= Capacity) {
            return false; // The buffer is full! We are dropping packets.
        }

        // Fast modulo using bitwise AND
        buffer[current_write & Mask] = item;
        
        // Release store guarantees the payload is fully written to memory 
        // BEFORE the consumers are allowed to see the updated write_index
        write_index.store(current_write + 1, std::memory_order_release);
        
        return true;
    }

    // ---------------------------------------------------------
    // CONSUMERS (Cores 1, 2, 3 - The Strategy Threads)
    // ---------------------------------------------------------
    bool pop(Itch5AddOrder& out_item) {
        size_t current_read = read_index.load(std::memory_order_relaxed);
        
        // Acquire load to safely read the producer's write_index
        if (current_read == write_index.load(std::memory_order_acquire)) {
            return false; // Buffer is empty. Thread should spin/wait.
        }

        // SPMC TRAP: Multiple consumers might see the same current_read index.
        // We use compare_exchange_strong to force them to fight for it.
        // Only ONE thread will successfully swap the index and win the payload.
        if (read_index.compare_exchange_strong(current_read, current_read + 1, std::memory_order_acq_rel)) {
            out_item = buffer[current_read & Mask];
            return true;
        }

        return false; // Another consumer beat us to it.
    }
};