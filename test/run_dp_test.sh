#!/bin/sh
# Compare our DeltaPlus encoder with HPLIP's. Usage: test/run_dp_test.sh <hplip-src-dir> [bands.bin slot]...
set -e
HPLIP=${1:?usage: $0 <hplip-src-dir> [bands.bin slot]...}
shift
HPCUPS="$HPLIP/prnt/hpcups"
cd "$(dirname "$0")/.."
mkdir -p build
clang -O2 -c src/deltaplus.c -o build/deltaplus.o
clang++ -std=c++11 -O2 -w -I"$HPCUPS" -o build/dp_vs_hplip \
    test/dp_vs_hplip.cpp "$HPCUPS/ModeDeltaPlus.cpp" "$HPCUPS/Compressor.cpp" "$HPCUPS/Processor.cpp" build/deltaplus.o -lcups
./build/dp_vs_hplip "$@"
