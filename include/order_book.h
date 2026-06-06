#pragma once
#include "itch_messages.h"
#include <cstdint>
#include <cstring>
#include <array>
#include <unordered_map>

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

    std::unordered_map<uint64_t, LiveOrder> live_orders_;
    Stats stats_;

    void handle_add(const DecodedOrder& order) {
        stats_.adds++;

        LiveOrder lo{order.order_ref, order.price, order.shares, order.side};
        live_orders_[order.order_ref] = lo;

        if (order.side == 'B') {
            add_to_levels(bid_levels_, bid_level_count_, order.price, order.shares);
            if (order.price > best_bid_price_) {
                best_bid_price_ = order.price;
                stats_.bbo_changes++;
            }
        } else {
            add_to_levels(ask_levels_, ask_level_count_, order.price, order.shares);
            if (best_ask_price_ == 0 || order.price < best_ask_price_) {
                best_ask_price_ = order.price;
                stats_.bbo_changes++;
            }
        }
    }

    void handle_execute(const DecodedOrder& order) {
        stats_.executes++;
        auto it = live_orders_.find(order.order_ref);
        if (it == live_orders_.end()) return;

        LiveOrder& lo = it->second;
        uint32_t exec_shares = order.shares;
        if (exec_shares > lo.remaining_shares) exec_shares = lo.remaining_shares;

        if (lo.side == 'B') {
            remove_from_levels(bid_levels_, bid_level_count_, lo.price, exec_shares);
        } else {
            remove_from_levels(ask_levels_, ask_level_count_, lo.price, exec_shares);
        }

        lo.remaining_shares -= exec_shares;
        if (lo.remaining_shares == 0) {
            char side = lo.side;
            uint32_t price = lo.price;
            live_orders_.erase(it);
            recalculate_bbo(side, price);
        }
    }

    void handle_delete(const DecodedOrder& order) {
        stats_.deletes++;
        auto it = live_orders_.find(order.order_ref);
        if (it == live_orders_.end()) return;

        LiveOrder& lo = it->second;
        if (lo.side == 'B') {
            remove_from_levels(bid_levels_, bid_level_count_, lo.price, lo.remaining_shares);
        } else {
            remove_from_levels(ask_levels_, ask_level_count_, lo.price, lo.remaining_shares);
        }

        char side = lo.side;
        uint32_t price = lo.price;
        live_orders_.erase(it);
        recalculate_bbo(side, price);
    }

    void handle_replace(const DecodedOrder& order) {
        stats_.replaces++;
        auto it = live_orders_.find(order.order_ref);
        if (it == live_orders_.end()) return;

        LiveOrder& lo = it->second;
        char side = lo.side;

        // Remove old
        if (side == 'B') {
            remove_from_levels(bid_levels_, bid_level_count_, lo.price, lo.remaining_shares);
        } else {
            remove_from_levels(ask_levels_, ask_level_count_, lo.price, lo.remaining_shares);
        }
        live_orders_.erase(it);

        // Add new
        LiveOrder new_lo{order.new_order_ref, order.price, order.shares, side};
        live_orders_[order.new_order_ref] = new_lo;

        if (side == 'B') {
            add_to_levels(bid_levels_, bid_level_count_, order.price, order.shares);
        } else {
            add_to_levels(ask_levels_, ask_level_count_, order.price, order.shares);
        }

        recalculate_bbo_full();
    }

    void add_to_levels(std::array<PriceLevel, MAX_LEVELS>& levels, size_t& count,
                       uint32_t price, uint32_t shares) {
        for (size_t i = 0; i < count; i++) {
            if (levels[i].price == price) {
                levels[i].total_shares += shares;
                levels[i].order_count++;
                return;
            }
        }
        if (count < MAX_LEVELS) {
            levels[count] = {price, shares, 1};
            count++;
        }
    }

    void remove_from_levels(std::array<PriceLevel, MAX_LEVELS>& levels, size_t& count,
                            uint32_t price, uint32_t shares) {
        for (size_t i = 0; i < count; i++) {
            if (levels[i].price == price) {
                if (shares >= levels[i].total_shares) {
                    levels[i] = levels[count - 1];
                    count--;
                } else {
                    levels[i].total_shares -= shares;
                    levels[i].order_count--;
                }
                return;
            }
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

    void recalculate_best_bid() {
        best_bid_price_ = 0;
        for (size_t i = 0; i < bid_level_count_; i++) {
            if (levels_valid(bid_levels_[i]) && bid_levels_[i].price > best_bid_price_)
                best_bid_price_ = bid_levels_[i].price;
        }
    }

    void recalculate_best_ask() {
        best_ask_price_ = 0;
        for (size_t i = 0; i < ask_level_count_; i++) {
            if (levels_valid(ask_levels_[i])) {
                if (best_ask_price_ == 0 || ask_levels_[i].price < best_ask_price_)
                    best_ask_price_ = ask_levels_[i].price;
            }
        }
    }

    static bool levels_valid(const PriceLevel& lvl) {
        return lvl.total_shares > 0;
    }
};

// Sharded book manager — each shard owns a subset of symbols
class ShardedBookManager {
public:
    static constexpr size_t MAX_SYMBOLS_PER_SHARD = 512;

    ShardedBookManager() = default;

    void apply(const DecodedOrder& order) {
        auto it = book_map_.find(order.ticker_key);
        if (it == book_map_.end()) {
            if (book_count_ >= MAX_SYMBOLS_PER_SHARD) return;
            book_map_[order.ticker_key] = book_count_;
            it = book_map_.find(order.ticker_key);
            book_count_++;
        }
        books_[it->second].apply(order);
        total_messages_++;
    }

    size_t total_messages() const { return total_messages_; }
    size_t symbol_count() const { return book_count_; }

    const OrderBook* get_book(uint64_t ticker_key) const {
        auto it = book_map_.find(ticker_key);
        if (it == book_map_.end()) return nullptr;
        return &books_[it->second];
    }

private:
    std::unordered_map<uint64_t, size_t> book_map_;
    std::array<OrderBook, MAX_SYMBOLS_PER_SHARD> books_;
    size_t book_count_ = 0;
    size_t total_messages_ = 0;
};
