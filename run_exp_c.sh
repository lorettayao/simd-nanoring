#!/bin/bash
set -e

echo "=== SCALING EXPERIMENT: Pipeline with varying thread counts ==="

mkdir -p build
cd build
cmake .. 2>/dev/null
make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd ..

echo ""
echo "--- 1 Book Thread ---"
./build/simd_nanoring 1 20000000

echo ""
echo "--- 2 Book Threads ---"
./build/simd_nanoring 2 20000000

echo ""
echo "--- 4 Book Threads ---"
./build/simd_nanoring 4 20000000

echo ""
echo "--- 8 Book Threads ---"
./build/simd_nanoring 8 20000000

echo ""
echo "=== Experiment Complete ==="
