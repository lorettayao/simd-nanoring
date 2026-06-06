#pragma once
#include <atomic>
#include <vector>
#include <cstddef>

// The Zero-Copy Broadcast Architecture (LMAX Disruptor Pattern)
// Templatized on capacity; payload type is determined by the journal's vector element.
// Consumers only READ the published index → MESI cache line stays in Shared (S) state.
template <size_t Capacity>
class BroadcastJournal {
private:
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    static constexpr size_t Mask = Capacity - 1;

    // Use a flat byte array to avoid requiring default-constructible payloads
    struct alignas(64) Slot {
        alignas(8) char data[64]; // large enough for DecodedOrder (≤48 bytes)
    };

    std::vector<Slot> journal_;

    // Single source of truth: the monotonically increasing sequence number.
    // alignas(64) isolates on its own cache line to prevent false sharing.
    alignas(64) std::atomic<size_t> published_index_{0};

    // Pad to prevent consumer reads from sharing the producer's cache line
    alignas(64) char pad_[64] = {};

public:
    BroadcastJournal() : journal_(Capacity) {}

    // PRODUCER: Write payload then release-fence the index
    template <typename T>
    inline void publish(size_t sequence, const T& item) {
        static_assert(sizeof(T) <= sizeof(Slot), "Payload exceeds slot size");
        __builtin_memcpy(&journal_[sequence & Mask], &item, sizeof(T));

        // Release: payload committed before consumers see updated index
        published_index_.store(sequence + 1, std::memory_order_release);
    }

    // CONSUMER: Acquire-load the published index (read-only, MESI Shared state)
    inline size_t get_published_index() const {
        return published_index_.load(std::memory_order_acquire);
    }

    // CONSUMER: O(1) zero-copy payload retrieval
    template <typename T>
    inline const T& get_payload(size_t sequence) const {
        return *reinterpret_cast<const T*>(&journal_[sequence & Mask]);
    }

    // Non-template convenience for the common case
    template <typename T>
    inline void publish_typed(size_t sequence, const T& item) {
        publish(sequence, item);
    }
};
