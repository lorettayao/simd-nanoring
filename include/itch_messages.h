#pragma once
#include <cstdint>
#include <cstring>

#pragma pack(push, 1)

struct ItchHeader {
    uint16_t length;
    char message_type;
};

// Type 'A' — Add Order (36 bytes payload after length prefix)
struct ItchAddOrder {
    char message_type;           // 'A'
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];
    uint64_t order_ref_num;
    char buy_sell_indicator;
    uint32_t shares;
    char stock[8];
    uint32_t price;              // 4 implied decimal places
};

// Type 'E' — Order Executed (30 bytes)
struct ItchOrderExecuted {
    char message_type;           // 'E'
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];
    uint64_t order_ref_num;
    uint32_t executed_shares;
    uint64_t match_number;
};

// Type 'D' — Order Delete (18 bytes)
struct ItchOrderDelete {
    char message_type;           // 'D'
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];
    uint64_t order_ref_num;
};

// Type 'U' — Order Replace (34 bytes)
struct ItchOrderReplace {
    char message_type;           // 'U'
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint8_t timestamp[6];
    uint64_t original_order_ref;
    uint64_t new_order_ref;
    uint32_t shares;
    uint32_t price;
};

#pragma pack(pop)

// Canonical internal representation after decoding
struct DecodedOrder {
    enum class Type : uint8_t { Add = 0, Execute, Delete, Replace };
    Type type;
    uint64_t order_ref;
    uint64_t ticker_key;      // 8-byte stock symbol as integer for fast comparison
    uint32_t price;
    uint32_t shares;
    char side;                // 'B' or 'S'
    uint64_t new_order_ref;   // only for Replace
};

inline uint64_t ticker_to_key(const char* stock) {
    uint64_t key = 0;
    std::memcpy(&key, stock, 8);
    return key;
}
