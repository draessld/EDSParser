#!/bin/bash

# Generate Patterns from EDS Files
# Uses edsparser-genpatterns to create random patterns for benchmarking

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default parameters
DATASET_NAME=""
INPUT_DIR="eds"
FILE_PATTERN="*"
PATTERN_COUNT=100
PATTERN_LENGTH=10
FORCE_OVERWRITE=false

# Tool path
GENPATTERNS_TOOL="edsparser-genpatterns"

# Statistics
SUCCESS_COUNT=0
FAILURE_COUNT=0
declare -a FAILED_FILES

# Helper functions
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1" >&2
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1" >&2
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1" >&2
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1" >&2
}

show_help() {
    cat << EOF
Usage: $0 --dataset DATASET [OPTIONS]

Generate random patterns from EDS files for benchmarking.

REQUIRED ARGUMENTS:
  --dataset DATASET   Dataset name (e.g., "SARS_cov2")

OPTIONS:
  --input-dir DIR     Input directory name (default: "eds")
  --pattern PATTERN   File pattern to process (default: "*")
  --count N           Number of patterns to generate per file (default: 100)
  --length L          Pattern length (default: 10)
  --force             Overwrite existing pattern files
  -h, --help          Show this help message

OUTPUT:
  Patterns are organized in folders inside the input directory:
    datasets/DATASET_NAME/<input_dir>/patterns_<count>_<length>/
      ├── file1.patterns
      ├── file2.patterns
      └── ...

  Examples:
    - datasets/SARS_cov2/eds/patterns_100_10/ - Patterns from EDS files
    - datasets/SARS_cov2/3_leds/patterns_100_10/ - Patterns from l-EDS (l=3)

EXAMPLES:
  # Generate 100 patterns of length 10 from all EDS files
  $0 --dataset SARS_cov2

  # Generate 1000 patterns of length 20
  $0 --dataset SARS_cov2 --count 1000 --length 20

  # Generate patterns from l-EDS files
  $0 --dataset SARS_cov2 --input-dir 3_leds --count 500 --length 15

  # Generate patterns from specific files
  $0 --dataset SARS_cov2 --pattern "20*"

PATTERN FILE FORMAT:
  Plain text file with one pattern per line:
    ACGTACGTAC
    TGCATGCATG
    ...

EOF
}

check_tool() {
    log_info "Checking for edsparser-genpatterns tool..."

    if ! command -v "$GENPATTERNS_TOOL" &> /dev/null; then
        log_error "Tool '$GENPATTERNS_TOOL' not found"
        log_error "Please run INSTALL.sh or ensure tools are installed"
        exit 1
    fi

    log_success "Found $GENPATTERNS_TOOL: $(which $GENPATTERNS_TOOL)"
}

generate_patterns() {
    local eds_file="$1"
    local dataset_path="$2"
    local input_dir_name="$3"
    local basename=$(basename "$eds_file" | sed -E 's/\.(eds|leds)$//')

    # Create pattern folder inside the input directory: <input_dir>/patterns_<count>_<length>/
    local input_dir_path="${dataset_path}/${input_dir_name}"
    local pattern_folder="${input_dir_path}/patterns_${PATTERN_COUNT}_${PATTERN_LENGTH}"
    mkdir -p "$pattern_folder"

    # Create output filename in pattern folder
    local output_file="${pattern_folder}/${basename}.patterns"

    # Skip if exists and not forcing overwrite
    if [[ -f "$output_file" ]] && [[ "$FORCE_OVERWRITE" == "false" ]]; then
        log_warning "Skipping $basename (patterns already exist)"
        return 0
    fi

    log_info "Generating patterns from $basename..."

    local start_time=$SECONDS

    # Generate patterns
    if $GENPATTERNS_TOOL \
        --input "$eds_file" \
        --output "$output_file" \
        --count "$PATTERN_COUNT" \
        --length "$PATTERN_LENGTH" \
        > /dev/null 2>&1; then

        local elapsed=$((SECONDS - start_time))
        local pattern_count=$(wc -l < "$output_file" 2>/dev/null || echo "0")

        log_success "Generated $pattern_count patterns for $basename (${elapsed}s)"
        ((SUCCESS_COUNT++))
        return 0
    else
        log_error "Failed to generate patterns for $basename"
        FAILED_FILES+=("$basename")
        ((FAILURE_COUNT++))
        return 1
    fi
}

print_summary() {
    local dataset_path="$1"
    local pattern_folder="patterns_${PATTERN_COUNT}_${PATTERN_LENGTH}"

    echo ""
    log_info "================================================"
    log_info "PATTERN GENERATION SUMMARY"
    log_info "================================================"
    log_success "Successfully processed: $SUCCESS_COUNT files"

    if [[ $FAILURE_COUNT -gt 0 ]]; then
        log_error "Failed: $FAILURE_COUNT files"
        for failed in "${FAILED_FILES[@]}"; do
            log_error "  - $failed"
        done
    fi

    log_info "Pattern parameters:"
    log_info "  - Count per file: $PATTERN_COUNT"
    log_info "  - Pattern length: $PATTERN_LENGTH"
    log_info "  - Pattern folders created inside each input directory"
    echo ""
}

main() {
    # Validate required arguments
    if [[ -z "$DATASET_NAME" ]]; then
        log_error "Dataset name is required (--dataset)"
        echo ""
        show_help
        exit 1
    fi

    local dataset_path="datasets/$DATASET_NAME"

    if [[ ! -d "$dataset_path" ]]; then
        log_error "Dataset not found: $dataset_path"
        exit 1
    fi

    local input_path="$dataset_path/$INPUT_DIR"

    if [[ ! -d "$input_path" ]]; then
        log_error "Input directory not found: $input_path"
        log_error "Have you run the experiment to generate EDS files first?"
        exit 1
    fi

    log_info "Starting pattern generation"
    log_info "Dataset: $DATASET_NAME"
    log_info "Input directory: $INPUT_DIR"
    log_info "Pattern count: $PATTERN_COUNT"
    log_info "Pattern length: $PATTERN_LENGTH"
    log_info "File pattern: $FILE_PATTERN"
    echo ""

    # Check tool availability
    check_tool

    # Find EDS/l-EDS files
    local eds_files=("$input_path"/$FILE_PATTERN.eds "$input_path"/$FILE_PATTERN.leds)
    local found_files=()

    for file in "${eds_files[@]}"; do
        if [[ -f "$file" ]]; then
            found_files+=("$file")
        fi
    done

    if [[ ${#found_files[@]} -eq 0 ]]; then
        log_error "No EDS/l-EDS files found matching: $FILE_PATTERN"
        exit 1
    fi

    log_info "Found ${#found_files[@]} file(s) to process"
    echo ""

    # Process each file
    for eds_file in "${found_files[@]}"; do
        generate_patterns "$eds_file" "$dataset_path" "$INPUT_DIR"
    done

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
        --input-dir)
            INPUT_DIR="$2"
            shift 2
            ;;
        --pattern)
            FILE_PATTERN="$2"
            shift 2
            ;;
        --count)
            PATTERN_COUNT="$2"
            shift 2
            ;;
        --length)
            PATTERN_LENGTH="$2"
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
