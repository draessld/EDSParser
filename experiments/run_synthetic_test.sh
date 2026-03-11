#!/bin/bash

# This script provides an end-to-end test of the experiment framework
# using a small, artificially generated dataset.

# It performs the following steps:
# 1. Generates a new synthetic dataset using generate_random_dataset.sh.
# 2. Transforms the generated EDS files into l-EDS formats using transform_to_eds.sh.
# 3. Generates statistics for all created files using generate_statistics.sh.
# 4. Generates test patterns from the EDS and l-EDS files using generate_patterns.sh.
# 5. Prints a summary and instructions for cleanup.

set -e # Exit immediately if a command exits with a non-zero status.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

# --- Configuration ---
DATASET_NAME="synthetic_test_$(date +%s)"
NUM_FILES=3
REF_SIZE_MB=1
VARIABILITY=0.01
LEDS_LENGTHS="3,5"
PATTERN_COUNT=50
PATTERN_LENGTH=10
THREADS=2

# --- Main Logic ---
main() {
    log_info "Starting End-to-End Synthetic Data Experiment"
    log_info "============================================="
    log_info "Dataset to be created: $DATASET_NAME"
    echo ""

    # --- Step 1: Generate Random Dataset ---
    log_info "STEP 1: Generating random EDS dataset..."
    ./generate_random_dataset.sh \
        --dataset "$DATASET_NAME" \
        --num-files "$NUM_FILES" \
        --ref-size-mb "$REF_SIZE_MB" \
        --variability "$VARIABILITY" \
        --threads "$THREADS" \
        --force
    echo ""

    # --- Step 2: Transform EDS to l-EDS ---
    log_info "STEP 2: Transforming EDS to l-EDS..."
    ./transform_to_eds.sh \
        --dataset "$DATASET_NAME" \
        --format eds \
        --lengths "$LEDS_LENGTHS" \
        --threads "$THREADS" \
        --force
    echo ""

    # --- Step 3: Generate Statistics ---
    log_info "STEP 3: Generating statistics..."
    local LEDS_DIRS
    LEDS_DIRS=$(echo "$LEDS_LENGTHS" | sed 's/,/_leds,/g' | sed 's/$/_leds/')
    ./generate_statistics.sh \
        --dataset "$DATASET_NAME" \
        --input-dirs "eds,$LEDS_DIRS" \
        --format csv \
        --output "datasets/$DATASET_NAME/${DATASET_NAME}_statistics.csv" \
        --threads "$THREADS" \
        --force
    echo ""

    # --- Step 4: Generate Patterns ---
    log_info "STEP 4: Generating patterns..."
    ./generate_patterns.sh \
        --dataset "$DATASET_NAME" \
        --input-dirs "eds,$LEDS_DIRS" \
        --count "$PATTERN_COUNT" \
        --length "$PATTERN_LENGTH" \
        --threads "$THREADS" \
        --force
    echo ""

    # --- Summary ---
    log_success "Experiment finished successfully!"
    log_info "============================================="
    log_info "Summary of created artifacts:"
    log_info "  - Dataset Path: datasets/$DATASET_NAME/"
    log_info "  - EDS files: $NUM_FILES in 'datasets/$DATASET_NAME/eds/'"
    log_info "  - l-EDS directories: $LEDS_DIRS"
    log_info "  - Statistics report: datasets/$DATASET_NAME/${DATASET_NAME}_statistics.csv"
    log_info "  - Pattern files created in 'patterns_${PATTERN_COUNT}_${PATTERN_LENGTH}/' subdirectories."
    echo ""
    log_info "To clean up all generated files, run:"
    log_warning "  ./clean_experiments.sh $DATASET_NAME"
    echo ""
}

# Run main function
main