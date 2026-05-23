#!/bin/bash
set -e

echo "=== Compiling Engine ==="
cd build
make -j$(nproc)
cd ..

echo "=== EXPERIMENT C: AMDAHL'S LAW SCALABILITY ==="

echo "-> Running with 1 Consumer Thread (Baseline)..."
./build/simd_nanoring 1

echo "-> Running with 2 Consumer Threads..."
./build/simd_nanoring 2

echo "-> Running with 4 Consumer Threads..."
./build/simd_nanoring 4

echo "=== Experiment Complete ==="