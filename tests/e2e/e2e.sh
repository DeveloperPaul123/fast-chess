#!/bin/bash 

set -x

# Compile the random_mover
g++ -O3 -std=c++17 tests/e2e/random_mover.cpp -o random_mover

# Compile fast-chess
make -j

# EPD Book Test

OUTPUT_FILE=$(mktemp)
./fast-chess -engine cmd=random_mover name=random_move_1 -engine cmd=random_mover name=random_move_2 \
    -each tc=2+0.02s -rounds 5 -repeat -concurrency 4 \
    -openings file=tests/e2e/openings.epd format=epd order=random | tee $OUTPUT_FILE

# Check if "Saved results." is in the output, else fail
if ! grep -q "Saved results." $OUTPUT_FILE; then
    echo "Failed to save results."
    exit 1
fi

# PGN Book Test

OUTPUT_FILE_2=$(mktemp)
./fast-chess -engine cmd=random_mover name=random_move_1 -engine cmd=random_mover name=random_move_2 \
    -each tc=2+0.02s -rounds 5 -repeat -concurrency 4 \
    -openings file=tests/e2e/openings.pgn format=pgn order=random | tee $OUTPUT_FILE_2

# Check if "Saved results." is in the output, else fail
if ! grep -q "Saved results." $OUTPUT_FILE_2; then
    echo "Failed to save results."
    exit 1
fi
