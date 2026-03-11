#!/bin/bash

# Generate quick stress test data (small files for ~2 minute total runtime)

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
DATA_DIR="$SCRIPT_DIR/data"

echo "Generating quick stress test data..."
echo "Data directory: $DATA_DIR"

mkdir -p "$DATA_DIR"

if ! command -v genrandomeds &> /dev/null; then
    echo "ERROR: genrandomeds not found. Please build and install EDSParser first."
    exit 1
fi

echo ""
echo "Generating 10MB test file..."
genrandomeds --ref-size-mb 10 --variability 0.01 \
    --min-alternatives 2 --max-alternatives 5 \
    -o "$DATA_DIR/test_10MB.eds"

echo ""
echo "Generating 50MB test file..."
genrandomeds --ref-size-mb 50 --variability 0.01 \
    --min-alternatives 2 --max-alternatives 5 \
    -o "$DATA_DIR/test_50MB.eds"

echo ""
echo "Quick test data generation complete!"
ls -lh "$DATA_DIR"/test_10MB.* "$DATA_DIR"/test_50MB.* 2>/dev/null
