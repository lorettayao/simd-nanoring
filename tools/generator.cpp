#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <random>
#include "itch_messages.h"

int main() {
    const char* filename = "mock_itch50.bin";
    std::ofstream outfile(filename, std::ios::binary);

    if (!outfile) {
        std::cerr << "Fatal Error: Cannot open " << filename << " for writing.\n";
        return 1;
    }

    const char* tickers[] = {
        "AAPL    ", "MSFT    ", "TSLA    ", "GOOG    ",
        "AMZN    ", "NFLX    ", "META    ", "NVDA    "
    };

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> ticker_dist(0, 7);
    std::uniform_int_distribution<uint32_t> price_dist(100000, 500000);

    const size_t TOTAL_MESSAGES = 10000000;
    std::cout << "Generating " << TOTAL_MESSAGES << " mock ITCH 5.0 messages...\n";

    ItchAddOrder msg{};
    msg.message_type = 'A';
    msg.buy_sell_indicator = 'B';
    msg.shares = __builtin_bswap32(100);

    for (size_t i = 0; i < TOTAL_MESSAGES; ++i) {
        std::memcpy(msg.stock, tickers[ticker_dist(gen)], 8);
        msg.price = __builtin_bswap32(price_dist(gen));
        msg.order_ref_num = i + 1;
        outfile.write(reinterpret_cast<const char*>(&msg), sizeof(ItchAddOrder));
    }

    outfile.close();
    std::cout << "Wrote " << (TOTAL_MESSAGES * sizeof(ItchAddOrder)) / (1024 * 1024)
              << " MB to " << filename << "\n";

    return 0;
}
