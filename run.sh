#!/bin/bash
set -e

echo "=== Building SIMD NanoRing ==="

mkdir -p build
cd build
cmake ..
make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd ..

echo ""
echo "=== Running Pipeline Demo ==="
./build/simd_nanoring 4 20000000

echo ""
echo "=== Running Experiment Suite ==="
./build/run_experiments
