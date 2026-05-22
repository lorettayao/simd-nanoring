#pragma once
#include <immintrin.h>
#include <cstdint>
#include <iostream>

// 1. Define the strictly aligned 36-byte mock data structure
#pragma pack(push, 1)
struct Itch5AddOrder {
    char message_type = 'A';      // Offset 0
    uint16_t stock_locate = 0;    // Offset 1
    uint16_t tracking_number = 0; // Offset 3
    uint8_t timestamp[6] = {0};   // Offset 5
    uint64_t order_ref_num = 0;   // Offset 11
    char buy_sell_indicator = 'B';// Offset 19
    uint32_t shares = 100;        // Offset 20
    char stock[8];                // Offset 24
    uint32_t price = 150000;      // Offset 32
};
#pragma pack(pop)

// 2. The AVX2 Bouncer
class SimdParser {
public:
    static void parse_batch(const Itch5AddOrder* messages, size_t num_messages, uint64_t target_ticker) {
        
        __m256i target_vec = _mm256_set1_epi64x(target_ticker);
        size_t matches_found = 0;

        for (size_t i = 0; i < num_messages - 4; i += 4) {
            
            uint64_t t0 = *reinterpret_cast<const uint64_t*>(messages[i].stock);
            uint64_t t1 = *reinterpret_cast<const uint64_t*>(messages[i+1].stock);
            uint64_t t2 = *reinterpret_cast<const uint64_t*>(messages[i+2].stock);
            uint64_t t3 = *reinterpret_cast<const uint64_t*>(messages[i+3].stock);

            __m256i incoming_vec = _mm256_set_epi64x(t3, t2, t1, t0);
            __m256i cmp_result = _mm256_cmpeq_epi64(incoming_vec, target_vec);
            int mask = _mm256_movemask_pd(_mm256_castsi256_pd(cmp_result));

            if (mask == 0) {
                continue; 
            }

            matches_found++;
        }

        std::cout << "SIMD Parser finished. Found " << matches_found << " AAPL trades in this batch.\n";
    }
};