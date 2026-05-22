#include <iostream>
#include <zlib.h>
#include <vector>

int main() {
    // Path to the downloaded ITCH 5.0 file in your directory
    const char* file_path = "10302019.NASDAQ_ITCH50.gz";

    // Open the compressed file directly for reading in binary mode ("rb")
    gzFile file = gzopen(file_path, "rb");
    if (!file) {
        std::cerr << "Fatal Error: Failed to open " << file_path << "\n";
        return 1;
    }

    // Create a 64KB buffer to hold our decompressed chunks in memory
    const size_t BUFFER_SIZE = 65536;
    std::vector<uint8_t> buffer(BUFFER_SIZE);

    std::cout << "Starting to stream the compressed ITCH 5.0 feed...\n";

    // Read the first chunk to prove the streaming works
    int bytes_read = gzread(file, buffer.data(), BUFFER_SIZE);
    
    if (bytes_read > 0) {
        std::cout << "Successfully decompressed " << bytes_read << " bytes into memory.\n";
        // This is exactly where we will point our AVX2 SIMD parser later!
    } else {
        std::cerr << "Failed to read data or EOF reached.\n";
    }

    gzclose(file);
    return 0;
}