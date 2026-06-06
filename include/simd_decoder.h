#pragma once
#include "itch_messages.h"
#include <cstddef>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
    #include <immintrin.h>
    #define SIMD_AVX2 1
#elif defined(__aarch64__) || defined(_M_ARM64)
    #include <arm_neon.h>
    #define SIMD_NEON 1
#else
    #define SIMD_SCALAR 1
#endif

// ITCH 5.0 message sizes (payload only, excluding 2-byte length prefix)
static constexpr size_t ITCH_ADD_ORDER_SIZE = 36;
static constexpr size_t ITCH_ORDER_EXECUTED_SIZE = 31;
static constexpr size_t ITCH_ORDER_DELETE_SIZE = 19;
static constexpr size_t ITCH_ORDER_REPLACE_SIZE = 35;

class SimdDecoder {
public:
    // Decode a batch of raw ITCH binary messages into DecodedOrder structs.
    // Returns the number of messages successfully decoded.
    // The input is a flat byte stream with 2-byte big-endian length prefixes.
    static size_t decode_stream(const uint8_t* stream, size_t stream_len,
                                DecodedOrder* output, size_t max_output) {
        size_t offset = 0;
        size_t count = 0;

        while (offset + 3 <= stream_len && count < max_output) {
            uint16_t msg_len = (static_cast<uint16_t>(stream[offset]) << 8) |
                                static_cast<uint16_t>(stream[offset + 1]);
            offset += 2;

            if (offset + msg_len > stream_len) break;

            char msg_type = static_cast<char>(stream[offset]);

            switch (msg_type) {
                case 'A': {
                    if (msg_len < ITCH_ADD_ORDER_SIZE) { offset += msg_len; continue; }
                    decode_add_order(stream + offset, output[count]);
                    count++;
                    break;
                }
                case 'E': {
                    if (msg_len < ITCH_ORDER_EXECUTED_SIZE) { offset += msg_len; continue; }
                    decode_execute(stream + offset, output[count]);
                    count++;
                    break;
                }
                case 'D': {
                    if (msg_len < ITCH_ORDER_DELETE_SIZE) { offset += msg_len; continue; }
                    decode_delete(stream + offset, output[count]);
                    count++;
                    break;
                }
                case 'U': {
                    if (msg_len < ITCH_ORDER_REPLACE_SIZE) { offset += msg_len; continue; }
                    decode_replace(stream + offset, output[count]);
                    count++;
                    break;
                }
                default:
                    break;
            }
            offset += msg_len;
        }
        return count;
    }

    // Batch-decode Add Orders using SIMD to extract ticker + price fields
    // Processes 4 messages at a time for vectorized field extraction.
    static size_t decode_add_orders_simd(const ItchAddOrder* msgs, size_t n,
                                         DecodedOrder* output) {
        size_t i = 0;
        size_t out_idx = 0;

#if defined(SIMD_NEON)
        // Process 4 Add Orders at a time using NEON
        for (; i + 4 <= n; i += 4) {
            // Load ticker fields (8 bytes each) into NEON registers
            uint64x2_t tickers_01 = vcombine_u64(
                vcreate_u64(load_ticker(msgs[i].stock)),
                vcreate_u64(load_ticker(msgs[i+1].stock))
            );
            uint64x2_t tickers_23 = vcombine_u64(
                vcreate_u64(load_ticker(msgs[i+2].stock)),
                vcreate_u64(load_ticker(msgs[i+3].stock))
            );

            // Load prices — ITCH prices are big-endian, byte-swap with NEON
            uint32x4_t prices_raw = {
                load_u32_be(&msgs[i].price),
                load_u32_be(&msgs[i+1].price),
                load_u32_be(&msgs[i+2].price),
                load_u32_be(&msgs[i+3].price)
            };
            // rev32 byte swap (big-endian to little-endian)
            uint32x4_t prices = vreinterpretq_u32_u8(
                vrev32q_u8(vreinterpretq_u8_u32(prices_raw))
            );

            // Load shares
            uint32x4_t shares_raw = {
                load_u32_be(&msgs[i].shares),
                load_u32_be(&msgs[i+1].shares),
                load_u32_be(&msgs[i+2].shares),
                load_u32_be(&msgs[i+3].shares)
            };
            uint32x4_t shares = vreinterpretq_u32_u8(
                vrev32q_u8(vreinterpretq_u8_u32(shares_raw))
            );

            // Store results — NEON lane indices must be compile-time constants
            uint32_t price_arr[4];
            vst1q_u32(price_arr, prices);
            uint32_t shares_arr[4];
            vst1q_u32(shares_arr, shares);
            uint64_t ticker_arr[4];
            vst1q_u64(ticker_arr, tickers_01);
            vst1q_u64(ticker_arr + 2, tickers_23);

            for (int j = 0; j < 4; j++) {
                output[out_idx].type = DecodedOrder::Type::Add;
                output[out_idx].side = msgs[i+j].buy_sell_indicator;
                output[out_idx].price = price_arr[j];
                output[out_idx].shares = shares_arr[j];
                output[out_idx].ticker_key = ticker_arr[j];
                std::memcpy(&output[out_idx].order_ref, &msgs[i+j].order_ref_num, 8);
                output[out_idx].new_order_ref = 0;
                out_idx++;
            }
        }
#elif defined(SIMD_AVX2)
        // Process 4 Add Orders at a time using AVX2
        for (; i + 4 <= n; i += 4) {
            // Load 4 ticker fields into a 256-bit register
            __m256i tickers = _mm256_set_epi64x(
                load_ticker(msgs[i+3].stock),
                load_ticker(msgs[i+2].stock),
                load_ticker(msgs[i+1].stock),
                load_ticker(msgs[i].stock)
            );

            // Load and byte-swap 4 prices using SSSE3 shuffle
            __m128i prices_raw = _mm_set_epi32(
                load_u32_be(&msgs[i+3].price),
                load_u32_be(&msgs[i+2].price),
                load_u32_be(&msgs[i+1].price),
                load_u32_be(&msgs[i].price)
            );
            __m128i bswap_mask = _mm_set_epi8(
                12,13,14,15, 8,9,10,11, 4,5,6,7, 0,1,2,3
            );
            __m128i prices = _mm_shuffle_epi8(prices_raw, bswap_mask);

            // Load and byte-swap 4 shares
            __m128i shares_raw = _mm_set_epi32(
                load_u32_be(&msgs[i+3].shares),
                load_u32_be(&msgs[i+2].shares),
                load_u32_be(&msgs[i+1].shares),
                load_u32_be(&msgs[i].shares)
            );
            __m128i shares = _mm_shuffle_epi8(shares_raw, bswap_mask);

            // Extract and store
            alignas(32) uint64_t ticker_arr[4];
            _mm256_store_si256((__m256i*)ticker_arr, tickers);
            alignas(16) uint32_t price_arr[4];
            _mm_store_si128((__m128i*)price_arr, prices);
            alignas(16) uint32_t shares_arr[4];
            _mm_store_si128((__m128i*)shares_arr, shares);

            for (int j = 0; j < 4; j++) {
                output[out_idx].type = DecodedOrder::Type::Add;
                output[out_idx].side = msgs[i+j].buy_sell_indicator;
                output[out_idx].price = price_arr[j];
                output[out_idx].shares = shares_arr[j];
                output[out_idx].ticker_key = ticker_arr[j];
                std::memcpy(&output[out_idx].order_ref, &msgs[i+j].order_ref_num, 8);
                output[out_idx].new_order_ref = 0;
                out_idx++;
            }
        }
#endif
        // Scalar tail
        for (; i < n; i++) {
            output[out_idx].type = DecodedOrder::Type::Add;
            output[out_idx].side = msgs[i].buy_sell_indicator;
            output[out_idx].ticker_key = load_ticker(msgs[i].stock);
            output[out_idx].order_ref = load_u64_unaligned(&msgs[i].order_ref_num);
            output[out_idx].price = swap32(load_u32_be(&msgs[i].price));
            output[out_idx].shares = swap32(load_u32_be(&msgs[i].shares));
            output[out_idx].new_order_ref = 0;
            out_idx++;
        }
        return out_idx;
    }

    // Vectorized ticker filtering — returns bitmask of matches in batch of 4
    static uint32_t filter_tickers_simd(const DecodedOrder* orders, size_t n,
                                         uint64_t target_key) {
        uint32_t match_mask = 0;
#if defined(SIMD_NEON)
        uint64x2_t target = vdupq_n_u64(target_key);
        for (size_t i = 0; i + 4 <= n; i += 4) {
            uint64x2_t t01 = vcombine_u64(
                vcreate_u64(orders[i].ticker_key),
                vcreate_u64(orders[i+1].ticker_key)
            );
            uint64x2_t t23 = vcombine_u64(
                vcreate_u64(orders[i+2].ticker_key),
                vcreate_u64(orders[i+3].ticker_key)
            );
            uint64x2_t cmp01 = vceqq_u64(t01, target);
            uint64x2_t cmp23 = vceqq_u64(t23, target);

            if (vgetq_lane_u64(cmp01, 0)) match_mask |= (1u << (i % 32));
            if (vgetq_lane_u64(cmp01, 1)) match_mask |= (1u << ((i+1) % 32));
            if (vgetq_lane_u64(cmp23, 0)) match_mask |= (1u << ((i+2) % 32));
            if (vgetq_lane_u64(cmp23, 1)) match_mask |= (1u << ((i+3) % 32));
        }
#elif defined(SIMD_AVX2)
        __m256i target = _mm256_set1_epi64x(target_key);
        for (size_t i = 0; i + 4 <= n; i += 4) {
            __m256i incoming = _mm256_set_epi64x(
                orders[i+3].ticker_key, orders[i+2].ticker_key,
                orders[i+1].ticker_key, orders[i].ticker_key
            );
            __m256i cmp = _mm256_cmpeq_epi64(incoming, target);
            int mask = _mm256_movemask_pd(_mm256_castsi256_pd(cmp));
            match_mask |= (static_cast<uint32_t>(mask) << (i % 32));
        }
#else
        for (size_t i = 0; i < n; i++) {
            if (orders[i].ticker_key == target_key) match_mask |= (1u << (i % 32));
        }
#endif
        return match_mask;
    }

private:
    static inline uint64_t load_ticker(const char* stock) {
        uint64_t val = 0;
        std::memcpy(&val, stock, 8);
        return val;
    }

    static inline uint32_t load_u32_be(const void* ptr) {
        uint32_t val;
        std::memcpy(&val, ptr, 4);
        return val;
    }

    static inline uint64_t load_u64_unaligned(const void* ptr) {
        uint64_t val;
        std::memcpy(&val, ptr, 8);
        return val;
    }

    static inline uint32_t swap32(uint32_t val) {
        return __builtin_bswap32(val);
    }

    static inline void decode_add_order(const uint8_t* raw, DecodedOrder& out) {
        const auto* msg = reinterpret_cast<const ItchAddOrder*>(raw);
        out.type = DecodedOrder::Type::Add;
        out.order_ref = load_u64_unaligned(&msg->order_ref_num);
        out.ticker_key = load_ticker(msg->stock);
        out.price = swap32(load_u32_be(&msg->price));
        out.shares = swap32(load_u32_be(&msg->shares));
        out.side = msg->buy_sell_indicator;
        out.new_order_ref = 0;
    }

    static inline void decode_execute(const uint8_t* raw, DecodedOrder& out) {
        const auto* msg = reinterpret_cast<const ItchOrderExecuted*>(raw);
        out.type = DecodedOrder::Type::Execute;
        out.order_ref = load_u64_unaligned(&msg->order_ref_num);
        out.shares = swap32(load_u32_be(&msg->executed_shares));
        out.ticker_key = 0;
        out.price = 0;
        out.side = 0;
        out.new_order_ref = 0;
    }

    static inline void decode_delete(const uint8_t* raw, DecodedOrder& out) {
        const auto* msg = reinterpret_cast<const ItchOrderDelete*>(raw);
        out.type = DecodedOrder::Type::Delete;
        out.order_ref = load_u64_unaligned(&msg->order_ref_num);
        out.ticker_key = 0;
        out.price = 0;
        out.shares = 0;
        out.side = 0;
        out.new_order_ref = 0;
    }

    static inline void decode_replace(const uint8_t* raw, DecodedOrder& out) {
        const auto* msg = reinterpret_cast<const ItchOrderReplace*>(raw);
        out.type = DecodedOrder::Type::Replace;
        out.order_ref = load_u64_unaligned(&msg->original_order_ref);
        out.new_order_ref = load_u64_unaligned(&msg->new_order_ref);
        out.price = swap32(load_u32_be(&msg->price));
        out.shares = swap32(load_u32_be(&msg->shares));
        out.ticker_key = 0;
        out.side = 0;
    }
};
