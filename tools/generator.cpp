#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <random>

// CRITICAL: We must force the compiler NOT to pad this struct. 
// It must be exactly 36 bytes to mimic the raw network protocol.
#pragma pack(push, 1)
struct Itch5AddOrder {
    char message_type = 'A';      // Offset 0
    uint16_t stock_locate = 0;    // Offset 1
    uint16_t tracking_number = 0; // Offset 3
    uint8_t timestamp[6] = {0};   // Offset 5
    uint64_t order_ref_num = 0;   // Offset 11
    char buy_sell_indicator = 'B';// Offset 19
    uint32_t shares = 100;        // Offset 20
    char stock[8];                // Offset 24: The SIMD Target
    uint32_t price = 150000;      // Offset 32: ITCH prices are implied 4 decimal places
};
#pragma pack(pop)

int main() {
    const char* filename = "mock_itch50.bin";
    std::ofstream outfile(filename, std::ios::binary);

    if (!outfile) {
        std::cerr << "Fatal Error: Cannot open " << filename << " for writing.\n";
        return 1;
    }

    // Tickers to randomize (8 bytes exactly, space-padded)
    const char* tickers[] = {
        "AAPL    ", "MSFT    ", "TSLA    ", "GOOG    ", 
        "AMZN    ", "NFLX    ", "META    ", "NVDA    "
    };

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(0, 7);

    // Let's generate 10 Million messages (~360 MB of raw binary data)
    const size_t TOTAL_MESSAGES = 10000000;
    
    std::cout << "Generating " << TOTAL_MESSAGES << " mock ITCH 5.0 messages...\n";

    Itch5AddOrder msg;
    for (size_t i = 0; i < TOTAL_MESSAGES; ++i) {
        // Assign a random ticker
        std::memcpy(msg.stock, tickers[distrib(gen)], 8);
        
        // Randomize the price slightly just to have varied data
        msg.price = 150000 + (distrib(gen) * 100); 

        // Write the exact 36 bytes to the binary file
        outfile.write(reinterpret_cast<const char*>(&msg), sizeof(Itch5AddOrder));
    }

    outfile.close();
    std::cout << "Success! Wrote " << (TOTAL_MESSAGES * sizeof(Itch5AddOrder)) / (1024 * 1024) 
              << " MB of pure binary data to " << filename << "\n";

    return 0;
}