#!/bin/bash

# Generate stress test data
# This script creates large EDS files for memory stress testing

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
DATA_DIR="$SCRIPT_DIR/data"
REPO_ROOT="$SCRIPT_DIR/../.."

echo "Generating stress test data..."
echo "Data directory: $DATA_DIR"

# Create data directory
mkdir -p "$DATA_DIR"

# Check if genrandomeds is available
if ! command -v genrandomeds &> /dev/null; then
    echo "ERROR: genrandomeds not found. Please build and install EDSParser first."
    echo "Run: ./INSTALL.sh"
    exit 1
fi

# Generate files
# Format: genrandomeds --ref-size-mb <SIZE> --variability 0.01 -o <OUTPUT>

echo ""
echo "Generating 100MB test file..."
genrandomeds --ref-size-mb 100 --variability 0.01 \
    --min-alternatives 2 --max-alternatives 5 \
    -o "$DATA_DIR/test_100MB.eds"

echo ""
echo "Generating 1GB test file..."
genrandomeds --ref-size-mb 1000 --variability 0.01 \
    --min-alternatives 2 --max-alternatives 5 \
    -o "$DATA_DIR/test_1GB.eds"

# Optional: Large files (commented out by default)
# Uncomment to generate 5GB and 10GB files

# echo ""
# echo "Generating 5GB test file (this will take a while)..."
# genrandomeds --ref-size-mb 5000 --variability 0.01 \
#     --min-alternatives 2 --max-alternatives 5 \
#     -o "$DATA_DIR/test_5GB.eds"

# echo ""
# echo "Generating 10GB test file (this will take a long time)..."
# genrandomeds --ref-size-mb 10000 --variability 0.01 \
#     --min-alternatives 2 --max-alternatives 5 \
#     -o "$DATA_DIR/test_10GB.eds"

echo ""
echo "Data generation complete!"
echo "Files created in: $DATA_DIR"
ls -lh "$DATA_DIR"
