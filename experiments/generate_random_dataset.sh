#!/bin/bash

# Generate Random EDS Dataset
# Creates synthetic EDS datasets with controlled parameters for benchmarking

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

DATASET_NAME=""
NUM_FILES=10
REF_SIZE_MB=1
VARIABILITY=0.10
MIN_ALTERNATIVES=2
MAX_ALTERNATIVES=4
VARIANT_LENGTH_MAX=10
SNP_RATIO=0.7
ALPHABET="ACGT"
MIN_CONTEXT=0
SEED=""
FORCE_OVERWRITE=false
THREADS=1
GENRANDOMEDS_TOOL=""
SUCCESS_COUNT=0
FAILURE_COUNT=0
declare -a FAILED_FILES

show_help() {
    cat << EOF
Usage: $0 --dataset DATASET [OPTIONS]

Generate synthetic EDS datasets with controlled parameters for benchmarking.

REQUIRED ARGUMENTS:
  --dataset DATASET       Dataset name or path
                          - Name: "synthetic_1MB" → datasets/synthetic_1MB/
                          - Relative path: "../my_datasets/test" → /absolute/path/to/my_datasets/test/
                          - Absolute path: "/data/eds_datasets/test" → /data/eds_datasets/test/

DATASET PARAMETERS:
  --num-files N           Number of EDS files to generate (default: 10)
  --ref-size-mb SIZE      Reference size in MB per file (default: 1)
  --variability FRAC      Fraction of variant positions, 0.0-1.0 (default: 0.10 = 10%)

VARIANT PARAMETERS:
  --min-alternatives N    Minimum alternatives per variant (default: 2)
  --max-alternatives N    Maximum alternatives per variant (default: 4)
  --variant-length-max N  Maximum indel length in bp (default: 10)
  --snp-ratio FRAC        Fraction of SNPs vs indels, 0.0-1.0 (default: 0.7 = 70%)
  --alphabet STR          Sequence alphabet (default: "ACGT")
  --min-context N         Minimum context between variants (default: 0, disabled)

OTHER OPTIONS:
  --seed N                Random seed for first file (incremented for each file)
  --threads N             Number of parallel jobs to run (default: 1)
  --force                 Overwrite existing files
  -h, --help              Show this help message

OUTPUT STRUCTURE:
  datasets/DATASET_NAME/
    └── eds/              # Generated EDS files with sources (.eds + .seds)
        ├── file_0.eds
        ├── file_0.seds
        ├── file_1.eds
        ├── file_1.seds
        └── ...

EXAMPLES:
  # Generate small test dataset (10 files × 1 MB)
  $0 --dataset synthetic_test

  # Generate medium dataset with high variability (20 files × 5 MB, 20% variants)
  $0 --dataset synthetic_medium --num-files 20 --ref-size-mb 5 --variability 0.20

  # Generate large dataset with custom parameters
  $0 --dataset synthetic_large \\
     --num-files 50 \\
     --ref-size-mb 10 \\
     --variability 0.15 \\
     --min-alternatives 2 \\
     --max-alternatives 6 \\
     --variant-length-max 20

  # Generate dataset optimized for l-EDS (minimum context enforced during generation)
  $0 --dataset synthetic_leds --min-context 10

  # Generate reproducible dataset with fixed seed
  $0 --dataset synthetic_reproducible --seed 42 --num-files 5

  # Generate to custom absolute path
  $0 --dataset /data/experiments/synthetic_custom --num-files 15

  # Generate to relative path
  $0 --dataset ../my_datasets/test_data --num-files 5

REPRODUCIBILITY:
  Use --seed to generate reproducible datasets. Each file uses seed + file_index.
  Example: --seed 100 generates files with seeds 100, 101, 102, ...

L-EDS TRANSFORMATION:
  This script generates only EDS files. To create l-EDS transformations, use:
  ./transform_to_eds.sh --dataset DATASET_NAME --format eds --lengths 3,5,10,15,20

PERFORMANCE NOTES:
  - Each MB takes ~1-3 seconds to generate depending on variability
  - Memory usage: ~2-3× reference size during generation

EOF
}

check_tool() {
    log_info "Locating required tools..."
    local build_dir="$SCRIPT_DIR/../build/src/cpp/tools"

    GENRANDOMEDS_TOOL=$(command -v genrandomeds || echo "$build_dir/genrandomeds")

    if [[ ! -x "$GENRANDOMEDS_TOOL" ]]; then
        log_error "Tool 'genrandomeds' not found or not executable."
        log_error "Please run ./INSTALL.sh from the project root."
        exit 1
    fi
}

check_tools() {
    check_tool

    log_success "All required tools found"
}

generate_single_eds() {
    local output_file="$1"
    local seed_value="$2"
    local file_idx="$3"

    local basename
    basename=$(basename "$output_file" .eds)

    # Skip if exists and not forcing overwrite
    if [[ -f "$output_file" ]] && [[ "$FORCE_OVERWRITE" == "false" ]]; then
        log_warning "Skipping $basename (already exists)"
        return 0
    fi

    log_info "Generating $basename (seed=$seed_value)..."

    local start_time=$SECONDS

    # Build genrandomeds command
    local cmd
    cmd="$GENRANDOMEDS_TOOL"
    cmd+=" --output \"$output_file\""
    cmd+=" --ref-size-mb $REF_SIZE_MB"
    cmd+=" --variability $VARIABILITY"
    cmd+=" --min-alternatives $MIN_ALTERNATIVES"
    cmd+=" --max-alternatives $MAX_ALTERNATIVES"
    cmd+=" --variant-length-max $VARIANT_LENGTH_MAX"
    cmd+=" --snp-ratio $SNP_RATIO"
    cmd+=" --alphabet \"$ALPHABET\""
    cmd+=" --min-context $MIN_CONTEXT"
    cmd+=" --seed $seed_value"

    # Execute with output to log file
    local log_file
    log_file="${output_file}.log"
    if eval "$cmd" > "$log_file" 2>&1; then
        local elapsed=$((SECONDS - start_time))
        log_success "Generated $basename (${elapsed}s)"
        rm -f "$log_file"  # Clean up log on success
        ((SUCCESS_COUNT++))
        return 0
    else
        log_error "Failed to generate $basename"
        log_error "See log: $log_file"
        FAILED_FILES+=("$basename")
        ((FAILURE_COUNT++))
        return 1
    fi
}

export -f generate_single_eds log_info log_success log_warning log_error


print_summary() {
    local dataset_path="$1"

    echo ""
    log_info "================================================"
    log_info "DATASET GENERATION SUMMARY"
    log_info "================================================"
    log_info "Dataset: $(get_display_name "$dataset_path")"
    log_info "Files generated: $SUCCESS_COUNT / $NUM_FILES"

    if [[ $FAILURE_COUNT -gt 0 ]]; then
        log_error "Failed: $FAILURE_COUNT files"
        for failed in "${FAILED_FILES[@]}"; do
            log_error "  - $failed"
        done
    fi

    echo ""
    log_info "Dataset parameters:"
    log_info "  Reference size: $REF_SIZE_MB MB per file"
    log_info "  Variability: $(echo "$VARIABILITY * 100" | bc)%"
    log_info "  Alternatives: [$MIN_ALTERNATIVES, $MAX_ALTERNATIVES]"
    log_info "  Max variant length: $VARIANT_LENGTH_MAX bp"
    log_info "  SNP ratio: $(echo "$SNP_RATIO * 100" | bc)%"

    if [[ $MIN_CONTEXT -gt 0 ]]; then
        log_info "  Minimum context: $MIN_CONTEXT bp (for l-EDS compatibility)"
    fi

    echo ""
    log_info "Output location: $dataset_path/"
    echo ""
}

get_display_name() {
    local path="$1"
    # If it's in datasets/, show just the name, otherwise show full path
    if [[ "$path" == datasets/* ]]; then
        basename "$path"
    else
        echo "$path"
    fi
}

main() {
    # Validate required arguments
    if [[ -z "$DATASET_NAME" ]]; then
        log_error "Dataset name is required (--dataset)"
        echo ""
        show_help
        exit 1
    fi

    # Resolve dataset path (supports both names and paths)
    dataset_path=$(resolve_dataset_path "$DATASET_NAME")
    export dataset_path
    local eds_dir
    eds_dir="${dataset_path}/eds"

    mkdir -p "$eds_dir"

    # Generate random seed if not provided
    if [[ -z "$SEED" ]]; then
        SEED=$RANDOM
        log_info "Using random seed: $SEED"
    fi

    log_info "Starting dataset generation"
    log_info "Dataset: $DATASET_NAME"
    log_info "Number of files: $NUM_FILES"
    log_info "Reference size: $REF_SIZE_MB MB per file"
    log_info "Variability: $(echo "$VARIABILITY * 100" | bc)%"
    log_info "Parallel jobs: $THREADS"
    echo ""

    # Check tools
    check_tools
    echo ""

    log_info "================================================"
    log_info "Generating EDS files"
    log_info "================================================"

    # Export variables for parallel execution
    export GENRANDOMEDS_TOOL FORCE_OVERWRITE REF_SIZE_MB VARIABILITY
    export MIN_ALTERNATIVES MAX_ALTERNATIVES VARIANT_LENGTH_MAX SNP_RATIO
    export ALPHABET MIN_CONTEXT

    # Generate EDS files
    # Create a sequence of numbers for xargs
    seq 0 $((NUM_FILES - 1)) | xargs -n 1 -P "$THREADS" -I {} \
        bash -c '
            i={}
            output_file="'"$eds_dir"'/file_${i}.eds"
            seed_value=$(('"$SEED"' + i))
            generate_single_eds "$output_file" "$seed_value" "$i"
        '

    # The old sequential way:
    # for ((i=0; i<NUM_FILES; i++)); do
    #     local output_file="${eds_dir}/file_${i}.eds"
    #     local seed_value=$((SEED + i))
    #     generate_single_eds "$output_file" "$seed_value" "$i"
    # done

    echo ""

    # Print summary
    print_summary "$dataset_path"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --dataset)
            DATASET_NAME="$2"
            shift 2
            ;;
        --num-files)
            NUM_FILES="$2"
            shift 2
            ;;
        --ref-size-mb)
            REF_SIZE_MB="$2"
            shift 2
            ;;
        --variability)
            VARIABILITY="$2"
            shift 2
            ;;
        --min-alternatives)
            MIN_ALTERNATIVES="$2"
            shift 2
            ;;
        --max-alternatives)
            MAX_ALTERNATIVES="$2"
            shift 2
            ;;
        --variant-length-max)
            VARIANT_LENGTH_MAX="$2"
            shift 2
            ;;
        --snp-ratio)
            SNP_RATIO="$2"
            shift 2
            ;;
        --alphabet)
            ALPHABET="$2"
            shift 2
            ;;
        --min-context)
            MIN_CONTEXT="$2"
            shift 2
            ;;
        --seed)
            SEED="$2"
            shift 2
            ;;
        --threads)
            THREADS="$2"
            shift 2
            ;;
        --force)
            FORCE_OVERWRITE=true
            shift
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            log_error "Unknown option: $1"
            echo ""
            show_help
            exit 1
            ;;
    esac
done

main
