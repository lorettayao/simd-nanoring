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

    struct alignas(64) Slot {
        alignas(8) char data[64];
    };

    std::vector<Slot> journal_;

    // Single source of truth: the monotonically increasing sequence number.
    // alignas(64) isolates on its own cache line to prevent false sharing.
    alignas(64) std::atomic<size_t> published_index_{0};

    // Slowest consumer's sequence — producer must not lap this.
    // Separate cache line from published_index_ to avoid false sharing.
    alignas(64) std::atomic<size_t> min_consumer_seq_{0};

    alignas(64) char pad_[64] = {};

public:
    static constexpr size_t capacity = Capacity;

    BroadcastJournal() : journal_(Capacity) {}

    // PRODUCER: Write payload then release-fence the index.
    // Returns false if publishing would lap the slowest consumer (backpressure).
    template <typename T>
    inline bool try_publish(size_t sequence, const T& item) {
        static_assert(sizeof(T) <= sizeof(Slot), "Payload exceeds slot size");

        size_t min_seq = min_consumer_seq_.load(std::memory_order_acquire);
        if (sequence - min_seq >= Capacity) {
            return false; // would overwrite unread slot
        }

        __builtin_memcpy(&journal_[sequence & Mask], &item, sizeof(T));
        published_index_.store(sequence + 1, std::memory_order_release);
        return true;
    }

    // PRODUCER: Publish unconditionally (use only when consumers are guaranteed to keep up)
    template <typename T>
    inline void publish(size_t sequence, const T& item) {
        static_assert(sizeof(T) <= sizeof(Slot), "Payload exceeds slot size");
        __builtin_memcpy(&journal_[sequence & Mask], &item, sizeof(T));
        published_index_.store(sequence + 1, std::memory_order_release);
    }

    // CONSUMER: Acquire-load the published index (read-only, MESI Shared state)
    inline size_t get_published_index() const {
        return published_index_.load(std::memory_order_acquire);
    }

    // CONSUMER: Report progress so producer knows the slowest reader position.
    // Each consumer should call this periodically (e.g., after draining a batch).
    inline void report_consumer_progress(size_t consumer_seq) {
        size_t current_min = min_consumer_seq_.load(std::memory_order_relaxed);
        while (consumer_seq > current_min) {
            if (min_consumer_seq_.compare_exchange_weak(
                    current_min, consumer_seq, std::memory_order_release)) {
                break;
            }
        }
    }

    // CONSUMER: O(1) zero-copy payload retrieval
    template <typename T>
    inline const T& get_payload(size_t sequence) const {
        return *reinterpret_cast<const T*>(&journal_[sequence & Mask]);
    }

    // Check how far ahead producer is of slowest consumer
    inline size_t backlog() const {
        return published_index_.load(std::memory_order_acquire) -
               min_consumer_seq_.load(std::memory_order_acquire);
    }
};
