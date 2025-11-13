#!/bin/bash

# Generate Statistics from EDS Files
# Uses edsparser-stats to collect and aggregate statistics from EDS/l-EDS files

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
INPUT_DIRS="eds"  # Comma-separated list of directories
FILE_PATTERN="*"
OUTPUT_FORMAT="table"  # table, json, csv
OUTPUT_FILE=""
FULL_MODE=false
FORCE_OVERWRITE=false

# Tool path
STATS_TOOL="edsparser-stats"

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

Generate statistics from EDS/l-EDS files using edsparser-stats.

REQUIRED ARGUMENTS:
  --dataset DATASET   Dataset name (e.g., "SARS_cov2")

OPTIONS:
  --input-dirs DIRS   Comma-separated list of input directories (default: "eds")
                      Examples: "eds", "3_leds,5_leds", "eds,3_leds,5_leds,10_leds"
  --pattern PATTERN   File pattern to process (default: "*")
  --format FORMAT     Output format: "table", "json", or "csv" (default: "table")
  --output FILE       Save output to file (default: stdout)
  --full              Use FULL mode (loads all strings, more detailed)
  --force             Overwrite existing output file
  -h, --help          Show this help message

OUTPUT FORMATS:
  table   - Human-readable table format (default)
  json    - JSON format (one object per file)
  csv     - CSV format (one row per file, requires 'jq' for full parsing)

EXAMPLES:
  # Generate table statistics for all EDS files
  $0 --dataset SARS_cov2

  # Generate statistics for multiple directories
  $0 --dataset SARS_cov2 --input-dirs "eds,3_leds,5_leds,10_leds"

  # Generate JSON statistics and save to file
  $0 --dataset SARS_cov2 --format json --output stats.json

  # Generate CSV statistics from l-EDS files
  $0 --dataset SARS_cov2 --input-dirs "3_leds" --format csv --output leds_stats.csv

  # Generate statistics from specific files
  $0 --dataset SARS_cov2 --pattern "20*"

  # Use FULL mode for detailed statistics
  $0 --dataset SARS_cov2 --full

CSV OUTPUT COLUMNS:
  file, input_dir, size_bytes, storage_mode, n_symbols, N_characters, m_strings,
  degenerate_symbols, regular_symbols, min_context_length, max_context_length,
  avg_context_length, total_change_size, common_characters, empty_strings,
  num_paths, max_paths_per_string, avg_paths_per_string, current_memory_bytes,
  estimated_full_memory_bytes, reduction_factor

EOF
}

check_tool() {
    log_info "Checking for edsparser-stats tool..."

    if ! command -v "$STATS_TOOL" &> /dev/null; then
        log_error "Tool '$STATS_TOOL' not found"
        log_error "Please run INSTALL.sh or ensure tools are installed"
        exit 1
    fi

    log_success "Found $STATS_TOOL: $(which $STATS_TOOL)"
}

print_csv_header() {
    echo "file,input_dir,size_bytes,storage_mode,n_symbols,N_characters,m_strings,degenerate_symbols,regular_symbols,min_context_length,max_context_length,avg_context_length,total_change_size,common_characters,empty_strings,num_paths,max_paths_per_string,avg_paths_per_string,current_memory_bytes,estimated_full_memory_bytes,reduction_factor"
}

parse_json_to_csv() {
    local json_output="$1"
    local filename="$2"
    local input_dir="$3"

    # Use jq if available, otherwise use grep/sed
    if command -v jq &> /dev/null; then
        echo "$json_output" | jq -r --arg file "$filename" --arg dir "$input_dir" '
            [
                $file,
                $dir,
                .file.size_bytes,
                .file.storage_mode,
                .structure.n_symbols,
                .structure.N_characters,
                .structure.m_strings,
                .structure.degenerate_symbols,
                .structure.regular_symbols,
                .context_lengths.min,
                .context_lengths.max,
                .context_lengths.avg,
                .variations.total_change_size,
                .variations.common_characters,
                .variations.empty_strings,
                .sources.num_paths,
                .sources.max_paths_per_string,
                .sources.avg_paths_per_string,
                .memory.current_bytes,
                .memory.estimated_full_bytes,
                .memory.reduction_factor
            ] | @csv
        ' | tr -d '"'
    else
        # Fallback: simple parsing (less robust)
        log_warning "jq not found, using basic parsing (install jq for better results)"
        echo "$filename,$input_dir,N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A,N/A"
    fi
}

generate_stats() {
    local eds_file="$1"
    local dataset_path="$2"
    local input_dir_name="$3"
    local basename=$(basename "$eds_file" | sed -E 's/\.(eds|leds)$//')

    # Find corresponding .seds file
    local seds_file="${eds_file%.eds}.seds"
    seds_file="${seds_file%.leds}.seds"

    log_info "Processing $input_dir_name/$basename..."

    local start_time=$SECONDS

    # Build command
    local cmd="$STATS_TOOL -i \"$eds_file\""

    if [[ -f "$seds_file" ]]; then
        cmd="$cmd -s \"$seds_file\""
    fi

    if [[ "$OUTPUT_FORMAT" == "json" ]]; then
        cmd="$cmd --json"
    fi

    if [[ "$FULL_MODE" == "true" ]]; then
        cmd="$cmd --full"
    fi

    # Execute and capture output
    local stats_output
    if stats_output=$(eval "$cmd" 2>&1); then
        local elapsed=$((SECONDS - start_time))

        # Output based on format
        if [[ "$OUTPUT_FORMAT" == "table" ]]; then
            echo "$stats_output" | grep -v "^\[Performance\]"
            echo ""
        elif [[ "$OUTPUT_FORMAT" == "json" ]]; then
            # Extract JSON (before performance line)
            echo "$stats_output" | sed '/^\[Performance\]/,$d'
        elif [[ "$OUTPUT_FORMAT" == "csv" ]]; then
            # Parse JSON to CSV
            local json_part=$(echo "$stats_output" | sed '/^\[Performance\]/,$d')
            parse_json_to_csv "$json_part" "$basename" "$input_dir_name"
        fi

        log_success "Processed $input_dir_name/$basename (${elapsed}s)" >&2
        ((SUCCESS_COUNT++))
        return 0
    else
        log_error "Failed to generate statistics for $input_dir_name/$basename"
        log_error "Output: $stats_output"
        FAILED_FILES+=("$input_dir_name/$basename")
        ((FAILURE_COUNT++))
        return 1
    fi
}

print_summary() {
    echo "" >&2
    log_info "================================================"
    log_info "STATISTICS GENERATION SUMMARY"
    log_info "================================================"
    log_success "Successfully processed: $SUCCESS_COUNT files"

    if [[ $FAILURE_COUNT -gt 0 ]]; then
        log_error "Failed: $FAILURE_COUNT files"
        for failed in "${FAILED_FILES[@]}"; do
            log_error "  - $failed"
        done
    fi

    log_info "Output format: $OUTPUT_FORMAT"
    if [[ -n "$OUTPUT_FILE" ]]; then
        log_info "Output saved to: $OUTPUT_FILE"
    fi
    echo "" >&2
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

    # Check if output file exists
    if [[ -n "$OUTPUT_FILE" ]] && [[ -f "$OUTPUT_FILE" ]] && [[ "$FORCE_OVERWRITE" == "false" ]]; then
        log_error "Output file already exists: $OUTPUT_FILE"
        log_error "Use --force to overwrite"
        exit 1
    fi

    log_info "Starting statistics generation"
    log_info "Dataset: $DATASET_NAME"
    log_info "Input directories: $INPUT_DIRS"
    log_info "File pattern: $FILE_PATTERN"
    log_info "Output format: $OUTPUT_FORMAT"
    log_info "Full mode: $FULL_MODE"
    echo ""

    # Check tool availability
    check_tool

    # Split input directories
    IFS=',' read -ra DIR_ARRAY <<< "$INPUT_DIRS"

    # Prepare output file if specified
    local output_redirect=""
    if [[ -n "$OUTPUT_FILE" ]]; then
        > "$OUTPUT_FILE"  # Create/truncate file
        output_redirect="> \"$OUTPUT_FILE\""
        log_info "Output will be saved to: $OUTPUT_FILE"
        echo ""
    fi

    # Print CSV header if needed
    if [[ "$OUTPUT_FORMAT" == "csv" ]]; then
        if [[ -n "$OUTPUT_FILE" ]]; then
            print_csv_header > "$OUTPUT_FILE"
        else
            print_csv_header
        fi
    fi

    # Process each directory
    for input_dir in "${DIR_ARRAY[@]}"; do
        input_dir=$(echo "$input_dir" | xargs)  # Trim whitespace

        local input_path="$dataset_path/$input_dir"

        if [[ ! -d "$input_path" ]]; then
            log_warning "Input directory not found: $input_path (skipping)"
            continue
        fi

        log_info "Processing directory: $input_dir"
        echo ""

        # Find EDS/l-EDS files
        local eds_files=("$input_path"/$FILE_PATTERN.eds "$input_path"/$FILE_PATTERN.leds)
        local found_files=()

        for file in "${eds_files[@]}"; do
            if [[ -f "$file" ]]; then
                found_files+=("$file")
            fi
        done

        if [[ ${#found_files[@]} -eq 0 ]]; then
            log_warning "No EDS/l-EDS files found in $input_dir matching: $FILE_PATTERN"
            continue
        fi

        log_info "Found ${#found_files[@]} file(s) in $input_dir"
        echo ""

        # Process each file
        for eds_file in "${found_files[@]}"; do
            if [[ -n "$OUTPUT_FILE" ]]; then
                generate_stats "$eds_file" "$dataset_path" "$input_dir" >> "$OUTPUT_FILE"
            else
                generate_stats "$eds_file" "$dataset_path" "$input_dir"
            fi
        done

        echo ""
    done

    # Print summary
    print_summary
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --dataset)
            DATASET_NAME="$2"
            shift 2
            ;;
        --input-dirs)
            INPUT_DIRS="$2"
            shift 2
            ;;
        --pattern)
            FILE_PATTERN="$2"
            shift 2
            ;;
        --format)
            OUTPUT_FORMAT="$2"
            shift 2
            ;;
        --output)
            OUTPUT_FILE="$2"
            shift 2
            ;;
        --full)
            FULL_MODE=true
            shift
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
