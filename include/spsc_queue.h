#pragma once
#include <atomic>
#include <vector>
#include <cstddef>
#include <cstring>

// Single-Producer Single-Consumer lock-free queue.
// Each shard gets its own queue — producer routes messages by symbol hash,
// consumer only reads messages that belong to it. Zero wasted cache loads.
template <size_t Capacity>
class SpscQueue {
private:
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    static constexpr size_t Mask = Capacity - 1;

    struct alignas(64) Slot {
        alignas(8) char data[64];
    };

    std::vector<Slot> slots_;

    // Producer's write position — only producer writes, consumer reads
    alignas(64) std::atomic<size_t> head_{0};

    // Consumer's read position — only consumer writes, producer reads
    alignas(64) std::atomic<size_t> tail_{0};

public:
    static constexpr size_t capacity = Capacity;

    SpscQueue() : slots_(Capacity) {}

    // PRODUCER: try to enqueue one item. Returns false if full.
    template <typename T>
    inline bool try_push(const T& item) {
        static_assert(sizeof(T) <= sizeof(Slot), "Payload exceeds slot size");

        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_acquire);

        if (h - t >= Capacity) return false; // full

        __builtin_memcpy(&slots_[h & Mask], &item, sizeof(T));
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // PRODUCER: spin-push (blocks until space available)
    template <typename T>
    inline void push(const T& item) {
        static_assert(sizeof(T) <= sizeof(Slot), "Payload exceeds slot size");

        size_t h = head_.load(std::memory_order_relaxed);
        while (h - tail_.load(std::memory_order_acquire) >= Capacity) {
            #if defined(__x86_64__) || defined(_M_X64)
                _mm_pause();
            #elif defined(__aarch64__) || defined(_M_ARM64)
                __asm__ volatile("yield" ::: "memory");
            #endif
        }

        __builtin_memcpy(&slots_[h & Mask], &item, sizeof(T));
        head_.store(h + 1, std::memory_order_release);
    }

    // CONSUMER: try to dequeue one item. Returns false if empty.
    template <typename T>
    inline bool try_pop(T& item) {
        static_assert(sizeof(T) <= sizeof(Slot), "Payload exceeds slot size");

        size_t t = tail_.load(std::memory_order_relaxed);
        size_t h = head_.load(std::memory_order_acquire);

        if (t >= h) return false; // empty

        __builtin_memcpy(&item, &slots_[t & Mask], sizeof(T));
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // CONSUMER: number of items available to read
    inline size_t available() const {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_relaxed);
        return h - t;
    }

    // PRODUCER: number of free slots
    inline size_t free_slots() const {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_acquire);
        return Capacity - (h - t);
    }
};
