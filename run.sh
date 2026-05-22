#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -e

echo "=== Building SIMD NanoRing ==="

# Create build directory if it doesn't exist
mkdir -p build
cd build

# Generate the Makefile via CMake
cmake ..

# Compile the code using all available CPU cores
make -j$(nproc)

echo "=== Execution Start ==="
# Run the compiled binary
./simd_nanoring