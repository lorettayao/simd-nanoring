#pragma once
#include <atomic>
#include <vector>
#include "simd_parser.h" 

// The Zero-Copy Broadcast Architecture (LMAX Disruptor Pattern)
template <size_t Capacity>
class BroadcastJournal {
private:
    // Capacity must be a power of 2 for fast bitwise modulo
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    static constexpr size_t Mask = Capacity - 1;

    std::vector<Itch5AddOrder> journal;

    // CRITICAL: The single source of truth. 
    // alignas(64) isolates this atomic on its own L1 cache line to prevent False Sharing.
    alignas(64) std::atomic<size_t> published_index{0};

public:
    BroadcastJournal() : journal(Capacity) {}

    // ---------------------------------------------------------
    // PRODUCER (Core 0)
    // ---------------------------------------------------------
    inline void publish(size_t sequence, const Itch5AddOrder& item) {
        // 1. Write the payload to the central arena
        journal[sequence & Mask] = item;
        
        // 2. The Sequence Barrier
        // memory_order_release guarantees the payload is fully committed to RAM 
        // BEFORE the consumers are allowed to see the updated index.
        published_index.store(sequence + 1, std::memory_order_release);
    }

    // ---------------------------------------------------------
    // CONSUMERS (Cores 1 to N)
    // ---------------------------------------------------------
    // Consumers spin-wait on this function.
    // Because they only READ, the MESI protocol keeps this cache line in the Shared (S) state.
    inline size_t get_published_index() const {
        return published_index.load(std::memory_order_acquire);
    }

    // O(1) Payload Retrieval
    inline const Itch5AddOrder& get_payload(size_t sequence) const {
        return journal[sequence & Mask];
    }
};