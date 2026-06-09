#pragma once
#include "itch_messages.h"
#include <cstdint>
#include <cstring>
#include <array>
#include <vector>

// A compact price level in the book
struct PriceLevel {
    uint32_t price = 0;
    uint32_t total_shares = 0;
    uint32_t order_count = 0;
};

// Per-order tracking for cancel/execute/replace
struct LiveOrder {
    uint64_t order_ref;
    uint32_t price;
    uint32_t remaining_shares;
    char side;
    bool occupied = false;
};

// Open-addressing hash map with linear probing.
// All entries stored inline in one contiguous allocation — no heap nodes,
// no pointer chasing. Typical lookup: 1 cache miss (vs 3+ for std::unordered_map).
class FlatOrderMap {
public:
    static constexpr size_t DEFAULT_CAPACITY = 1 << 20; // 1M slots (~24MB)
    static constexpr uint64_t EMPTY = 0;
    static constexpr uint64_t TOMBSTONE = ~uint64_t(0);

    FlatOrderMap() : capacity_(DEFAULT_CAPACITY), mask_(capacity_ - 1), size_(0) {
        slots_.resize(capacity_);
    }

    LiveOrder* find(uint64_t order_ref) {
        size_t idx = hash(order_ref) & mask_;
        for (size_t probe = 0; probe < capacity_; probe++) {
            LiveOrder& slot = slots_[idx];
            if (!slot.occupied && slot.order_ref == EMPTY) return nullptr;
            if (slot.occupied && slot.order_ref == order_ref) return &slot;
            idx = (idx + 1) & mask_;
        }
        return nullptr;
    }

    void insert(uint64_t order_ref, const LiveOrder& order) {
        if (size_ * 4 >= capacity_ * 3) rehash();
        size_t idx = hash(order_ref) & mask_;
        for (;;) {
            LiveOrder& slot = slots_[idx];
            if (!slot.occupied) {
                slot = order;
                slot.order_ref = order_ref;
                slot.occupied = true;
                size_++;
                return;
            }
            if (slot.order_ref == order_ref) {
                slot = order;
                slot.order_ref = order_ref;
                slot.occupied = true;
                return;
            }
            idx = (idx + 1) & mask_;
        }
    }

    void erase(uint64_t order_ref) {
        size_t idx = hash(order_ref) & mask_;
        for (size_t probe = 0; probe < capacity_; probe++) {
            LiveOrder& slot = slots_[idx];
            if (!slot.occupied && slot.order_ref == EMPTY) return;
            if (slot.occupied && slot.order_ref == order_ref) {
                slot.occupied = false;
                slot.order_ref = TOMBSTONE;
                size_--;
                return;
            }
            idx = (idx + 1) & mask_;
        }
    }

private:
    static inline size_t hash(uint64_t key) {
        key ^= key >> 33;
        key *= 0xff51afd7ed558ccdULL;
        key ^= key >> 33;
        key *= 0xc4ceb9fe1a85ec53ULL;
        key ^= key >> 33;
        return static_cast<size_t>(key);
    }

    void rehash() {
        size_t new_cap = capacity_ * 2;
        std::vector<LiveOrder> new_slots(new_cap);
        size_t new_mask = new_cap - 1;

        for (size_t i = 0; i < capacity_; i++) {
            if (slots_[i].occupied) {
                size_t idx = hash(slots_[i].order_ref) & new_mask;
                while (new_slots[idx].occupied) idx = (idx + 1) & new_mask;
                new_slots[idx] = slots_[i];
            }
        }
        slots_ = std::move(new_slots);
        capacity_ = new_cap;
        mask_ = new_mask;
    }

    std::vector<LiveOrder> slots_;
    size_t capacity_;
    size_t mask_;
    size_t size_;
};

// Single-symbol order book with sorted price levels
// Maintains top-of-book (BBO) for signal generation
class OrderBook {
public:
    static constexpr size_t MAX_LEVELS = 256;

    struct Stats {
        uint64_t adds = 0;
        uint64_t executes = 0;
        uint64_t deletes = 0;
        uint64_t replaces = 0;
        uint64_t bbo_changes = 0;
    };

    void apply(const DecodedOrder& order) {
        switch (order.type) {
            case DecodedOrder::Type::Add:
                handle_add(order);
                break;
            case DecodedOrder::Type::Execute:
                handle_execute(order);
                break;
            case DecodedOrder::Type::Delete:
                handle_delete(order);
                break;
            case DecodedOrder::Type::Replace:
                handle_replace(order);
                break;
        }
    }

    uint32_t best_bid() const { return best_bid_price_; }
    uint32_t best_ask() const { return best_ask_price_; }
    uint32_t spread() const {
        if (best_ask_price_ == 0 || best_bid_price_ == 0) return 0;
        return best_ask_price_ - best_bid_price_;
    }
    size_t bid_depth() const { return bid_level_count_; }
    size_t ask_depth() const { return ask_level_count_; }
    const Stats& stats() const { return stats_; }

private:
    std::array<PriceLevel, MAX_LEVELS> bid_levels_{};
    std::array<PriceLevel, MAX_LEVELS> ask_levels_{};
    size_t bid_level_count_ = 0;
    size_t ask_level_count_ = 0;

    uint32_t best_bid_price_ = 0;
    uint32_t best_ask_price_ = 0;

    FlatOrderMap live_orders_;
    Stats stats_;

    void handle_add(const DecodedOrder& order) {
        stats_.adds++;

        LiveOrder lo{order.order_ref, order.price, order.shares, order.side, true};
        live_orders_.insert(order.order_ref, lo);

        if (order.side == 'B') {
            add_to_levels(bid_levels_, bid_level_count_, order.price, order.shares, true);
            if (order.price > best_bid_price_) {
                best_bid_price_ = order.price;
                stats_.bbo_changes++;
            }
        } else {
            add_to_levels(ask_levels_, ask_level_count_, order.price, order.shares, false);
            if (best_ask_price_ == 0 || order.price < best_ask_price_) {
                best_ask_price_ = order.price;
                stats_.bbo_changes++;
            }
        }
    }

    void handle_execute(const DecodedOrder& order) {
        stats_.executes++;
        LiveOrder* lo = live_orders_.find(order.order_ref);
        if (!lo) return;

        uint32_t exec_shares = order.shares;
        if (exec_shares > lo->remaining_shares) exec_shares = lo->remaining_shares;

        if (lo->side == 'B') {
            remove_from_levels(bid_levels_, bid_level_count_, lo->price, exec_shares, true);
        } else {
            remove_from_levels(ask_levels_, ask_level_count_, lo->price, exec_shares, false);
        }

        lo->remaining_shares -= exec_shares;
        if (lo->remaining_shares == 0) {
            char side = lo->side;
            uint32_t price = lo->price;
            live_orders_.erase(order.order_ref);
            recalculate_bbo(side, price);
        }
    }

    void handle_delete(const DecodedOrder& order) {
        stats_.deletes++;
        LiveOrder* lo = live_orders_.find(order.order_ref);
        if (!lo) return;

        if (lo->side == 'B') {
            remove_from_levels(bid_levels_, bid_level_count_, lo->price, lo->remaining_shares, true);
        } else {
            remove_from_levels(ask_levels_, ask_level_count_, lo->price, lo->remaining_shares, false);
        }

        char side = lo->side;
        uint32_t price = lo->price;
        live_orders_.erase(order.order_ref);
        recalculate_bbo(side, price);
    }

    void handle_replace(const DecodedOrder& order) {
        stats_.replaces++;
        LiveOrder* lo = live_orders_.find(order.order_ref);
        if (!lo) return;

        char side = lo->side;

        // Remove old
        if (side == 'B') {
            remove_from_levels(bid_levels_, bid_level_count_, lo->price, lo->remaining_shares, true);
        } else {
            remove_from_levels(ask_levels_, ask_level_count_, lo->price, lo->remaining_shares, false);
        }
        live_orders_.erase(order.order_ref);

        // Add new
        LiveOrder new_lo{order.new_order_ref, order.price, order.shares, side, true};
        live_orders_.insert(order.new_order_ref, new_lo);

        if (side == 'B') {
            add_to_levels(bid_levels_, bid_level_count_, order.price, order.shares, true);
        } else {
            add_to_levels(ask_levels_, ask_level_count_, order.price, order.shares, false);
        }

        recalculate_bbo_full();
    }

    // Binary search for price in a sorted level array.
    // Bid levels: sorted descending (best bid first).
    // Ask levels: sorted ascending (best ask first).
    // Returns index where price is found, or insertion point (as negative - 1).
    static ssize_t binary_find(const std::array<PriceLevel, MAX_LEVELS>& levels,
                               size_t count, uint32_t price, bool descending) {
        ssize_t lo = 0, hi = static_cast<ssize_t>(count) - 1;
        while (lo <= hi) {
            ssize_t mid = (lo + hi) >> 1;
            if (levels[mid].price == price) return mid;
            bool go_right = descending ? (levels[mid].price > price)
                                       : (levels[mid].price < price);
            if (go_right) lo = mid + 1;
            else hi = mid - 1;
        }
        return -(lo + 1); // insertion point encoded as negative
    }

    void add_to_levels(std::array<PriceLevel, MAX_LEVELS>& levels, size_t& count,
                       uint32_t price, uint32_t shares, bool descending) {
        ssize_t idx = binary_find(levels, count, price, descending);
        if (idx >= 0) {
            levels[idx].total_shares += shares;
            levels[idx].order_count++;
            return;
        }
        if (count >= MAX_LEVELS) return;

        // Insert at sorted position
        size_t insert_pos = static_cast<size_t>(-(idx + 1));
        // Shift elements right to make room
        for (size_t i = count; i > insert_pos; i--) {
            levels[i] = levels[i - 1];
        }
        levels[insert_pos] = {price, shares, 1};
        count++;
    }

    void remove_from_levels(std::array<PriceLevel, MAX_LEVELS>& levels, size_t& count,
                            uint32_t price, uint32_t shares, bool descending) {
        ssize_t idx = binary_find(levels, count, price, descending);
        if (idx < 0) return;

        size_t i = static_cast<size_t>(idx);
        if (shares >= levels[i].total_shares) {
            // Remove level: shift elements left
            for (size_t j = i; j + 1 < count; j++) {
                levels[j] = levels[j + 1];
            }
            count--;
        } else {
            levels[i].total_shares -= shares;
            levels[i].order_count--;
        }
    }

    void recalculate_bbo(char side, uint32_t removed_price) {
        if (side == 'B' && removed_price == best_bid_price_) {
            recalculate_best_bid();
            stats_.bbo_changes++;
        } else if (side == 'S' && removed_price == best_ask_price_) {
            recalculate_best_ask();
            stats_.bbo_changes++;
        }
    }

    void recalculate_bbo_full() {
        recalculate_best_bid();
        recalculate_best_ask();
        stats_.bbo_changes++;
    }

    // Sorted descending — best bid is simply the first valid level
    void recalculate_best_bid() {
        best_bid_price_ = (bid_level_count_ > 0) ? bid_levels_[0].price : 0;
    }

    // Sorted ascending — best ask is simply the first valid level
    void recalculate_best_ask() {
        best_ask_price_ = (ask_level_count_ > 0) ? ask_levels_[0].price : 0;
    }
};

// Sharded book manager — each shard owns a subset of symbols.
// Uses a small open-addressed map for symbol→book index lookup.
class ShardedBookManager {
public:
    static constexpr size_t MAX_SYMBOLS_PER_SHARD = 512;
    static constexpr size_t SYMBOL_MAP_CAPACITY = 1024; // power of 2, load factor < 0.5

    ShardedBookManager() {
        symbol_keys_.resize(SYMBOL_MAP_CAPACITY, 0);
        symbol_vals_.resize(SYMBOL_MAP_CAPACITY, EMPTY_VAL);
    }

    void apply(const DecodedOrder& order) {
        size_t book_idx = find_or_insert_symbol(order.ticker_key);
        if (book_idx == FULL) return;
        books_[book_idx].apply(order);
        total_messages_++;
    }

    size_t total_messages() const { return total_messages_; }
    size_t symbol_count() const { return book_count_; }

    const OrderBook* get_book(uint64_t ticker_key) const {
        size_t mask = SYMBOL_MAP_CAPACITY - 1;
        size_t idx = symbol_hash(ticker_key) & mask;
        for (size_t probe = 0; probe < SYMBOL_MAP_CAPACITY; probe++) {
            if (symbol_vals_[idx] == EMPTY_VAL) return nullptr;
            if (symbol_keys_[idx] == ticker_key) return &books_[symbol_vals_[idx]];
            idx = (idx + 1) & mask;
        }
        return nullptr;
    }

private:
    static constexpr size_t EMPTY_VAL = ~size_t(0);
    static constexpr size_t FULL = ~size_t(0);

    std::vector<uint64_t> symbol_keys_;
    std::vector<size_t> symbol_vals_;
    std::array<OrderBook, MAX_SYMBOLS_PER_SHARD> books_;
    size_t book_count_ = 0;
    size_t total_messages_ = 0;

    static inline size_t symbol_hash(uint64_t key) {
        key ^= key >> 33;
        key *= 0xff51afd7ed558ccdULL;
        key ^= key >> 33;
        return static_cast<size_t>(key);
    }

    size_t find_or_insert_symbol(uint64_t ticker_key) {
        size_t mask = SYMBOL_MAP_CAPACITY - 1;
        size_t idx = symbol_hash(ticker_key) & mask;
        for (size_t probe = 0; probe < SYMBOL_MAP_CAPACITY; probe++) {
            if (symbol_vals_[idx] == EMPTY_VAL) {
                if (book_count_ >= MAX_SYMBOLS_PER_SHARD) return FULL;
                symbol_keys_[idx] = ticker_key;
                symbol_vals_[idx] = book_count_;
                book_count_++;
                return symbol_vals_[idx];
            }
            if (symbol_keys_[idx] == ticker_key) return symbol_vals_[idx];
            idx = (idx + 1) & mask;
        }
        return FULL;
    }
};
