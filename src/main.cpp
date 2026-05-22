#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include "simd_parser.h"

// Helper function to convert an 8-character string like "AAPL    " into a 64-bit integer
uint64_t ticker_to_uint64(const char* ticker) {
    uint64_t result = 0;
    std::memcpy(&result, ticker, 8);
    return result;
}

int main() {
    const char* filename = "../mock_itch50.bin"; // Adjust path if needed based on where you run it
    
    // 1. Open the raw binary file
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "Failed to open " << filename << ". Did you run the generator?\n";
        return 1;
    }

    // 2. Determine file size and allocate memory
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    size_t num_messages = size / sizeof(Itch5AddOrder);
    std::vector<Itch5AddOrder> market_data(num_messages);

    // 3. Blast the entire file into RAM at once
    std::cout << "Loading " << num_messages << " messages into memory...\n";
    if (!file.read(reinterpret_cast<char*>(market_data.data()), size)) {
        std::cerr << "Failed to read binary data.\n";
        return 1;
    }
    file.close();

    // 4. Fire up the SIMD Bouncer looking for Apple
    uint64_t target_aapl = ticker_to_uint64("AAPL    ");
    std::cout << "Starting AVX2 Vectorized Filtering...\n";
    
    SimdParser::parse_batch(market_data.data(), num_messages, target_aapl);

    return 0;
}