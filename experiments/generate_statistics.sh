#!/bin/bash

# Generate Statistics from EDS Files
# Uses edsparser-stats to collect and aggregate statistics from EDS/l-EDS files

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

DATASET_NAME=""
INPUT_DIRS="eds"  # Comma-separated list of directories
FILE_PATTERN="*"
OUTPUT_FORMAT="table"  # table, json, csv
OUTPUT_FILE=""
FULL_MODE=false
FORCE_OVERWRITE=false
THREADS=1
STATS_TOOL=""
SUCCESS_COUNT=0
FAILURE_COUNT=0
declare -a FAILED_FILES
declare -a OUTPUT_FILES

show_help() {
    cat << EOF
Usage: $0 --dataset DATASET [OPTIONS]

Generate statistics from EDS/l-EDS files using edsparser-stats.

REQUIRED ARGUMENTS:
  --dataset DATASET   Dataset name or path (e.g., "SARS_cov2" or "/data/my_dataset")

OPTIONS:
  --input-dirs DIRS   Comma-separated list of input directories (default: "eds")
                      Examples: "eds", "3_leds,5_leds", "eds,3_leds,5_leds,10_leds"
  --pattern PATTERN   File pattern to process (default: "*")
  --format FORMAT     Output format: "table", "json", or "csv" (default: "table")
  --threads N         Number of parallel jobs to run (default: 1). Not for 'table' format.
  --output FILE       Custom output filename (default: auto-generate per directory)
                      Auto-generated: datasets/DATASET/<dir_name>_statistics.<ext>
  --full              Use FULL mode (loads all strings, more detailed)
  --force             Overwrite existing output files
  -h, --help          Show this help message

OUTPUT FORMATS:
  table   - Human-readable table format (default)
  json    - JSON format (one object per file)
  csv     - CSV format (one row per file, requires 'jq' for full parsing)

EXAMPLES:
  # Generate CSV statistics for all EDS files (auto-saves to datasets/SARS_cov2/eds_statistics.csv)
  $0 --dataset SARS_cov2 --format csv

  # Generate statistics for multiple directories (creates one file per directory)
  $0 --dataset SARS_cov2 --input-dirs "eds,3_leds,5_leds,10_leds" --format csv
  # Creates: eds_statistics.csv, 3_leds_statistics.csv, 5_leds_statistics.csv, 10_leds_statistics.csv

  # Generate JSON statistics (auto-saves to datasets/SARS_cov2/eds_statistics.json)
  $0 --dataset SARS_cov2 --format json

  # Generate CSV statistics with custom output filename (all dirs in one file)
  $0 --dataset SARS_cov2 --input-dirs "eds,3_leds" --format csv --output all_stats.csv

  # Generate statistics from specific files
  $0 --dataset SARS_cov2 --pattern "20*" --format csv

  # Use FULL mode for detailed statistics
  $0 --dataset SARS_cov2 --format csv --full

CSV OUTPUT COLUMNS:
  file, input_dir, size_bytes, storage_mode, n_symbols, N_characters, m_strings,
  degenerate_symbols, regular_symbols, min_context_length, max_context_length,
  avg_context_length, total_change_size, common_characters, empty_strings,
  num_paths, max_paths_per_string, avg_paths_per_string, current_memory_bytes,
  estimated_full_memory_bytes, reduction_factor

EOF
}

check_tool() {
    log_info "Locating required tools..."
    local build_dir="$SCRIPT_DIR/../build/src/cpp/tools"

    STATS_TOOL=$(command -v edsparser-stats || echo "$build_dir/edsparser-stats")

    if [[ ! -x "$STATS_TOOL" ]]; then
        log_error "Tool 'edsparser-stats' not found or not executable."
        log_error "Please run ./INSTALL.sh from the project root."
        exit 1
    fi

    log_success "Found edsparser-stats: $STATS_TOOL"
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
    local input_dir_name="$2"
    local basename
    basename=$(basename "$eds_file")
    basename="${basename%.eds}"
    basename="${basename%.leds}"

    # Find corresponding .seds file
    local seds_file
    seds_file="${eds_file%.eds}.seds"
    seds_file="${seds_file%.leds}.seds"

    log_info "Processing $input_dir_name/$basename..."

    local start_time=$SECONDS

    # Build command
    local cmd="$STATS_TOOL -i \"$eds_file\""

    if [[ -f "$seds_file" ]]; then
        cmd="$cmd -s \"$seds_file\""
    fi

    if [[ "$OUTPUT_FORMAT" == "json" ]] || [[ "$OUTPUT_FORMAT" == "csv" ]]; then
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

# Export function for parallel execution
export -f generate_stats log_info log_success log_warning log_error parse_json_to_csv

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
    if [[ ${#OUTPUT_FILES[@]} -gt 0 ]]; then
        log_info "Output files created (${#OUTPUT_FILES[@]}):"
        for output in "${OUTPUT_FILES[@]}"; do
            log_info "  - $output"
        done
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

    # Resolve dataset path (supports both names and paths)
    dataset_path=$(resolve_dataset_path "$DATASET_NAME")
    export dataset_path

    if [[ ! -d "$dataset_path" ]]; then
        log_error "Dataset not found: $dataset_path"
        exit 1
    fi

    log_info "Starting statistics generation"
    log_info "Dataset: $DATASET_NAME"
    log_info "Input directories: $INPUT_DIRS"
    log_info "File pattern: $FILE_PATTERN"
    log_info "Output format: $OUTPUT_FORMAT"
    log_info "Parallel jobs: $THREADS"
    log_info "Full mode: $FULL_MODE"
    echo ""

    # Check tool availability
    check_tool

    # Split input directories
    local DIR_ARRAY
    IFS=',' read -ra DIR_ARRAY <<< "$INPUT_DIRS"

    # Determine file extension based on format
    local file_ext
    case "$OUTPUT_FORMAT" in
        json) file_ext="json" ;;
        csv)  file_ext="csv" ;;
        *)    file_ext="txt" ;;
    esac

    # Process each directory
    for input_dir in "${DIR_ARRAY[@]}"; do
        input_dir=$(echo "$input_dir" | xargs)  # Trim whitespace

        local input_path="$dataset_path/$input_dir"

        if [[ ! -d "$input_path" ]]; then
            log_warning "Input directory not found: $input_path (skipping)"
            continue
        fi

        # Determine output file for this directory
        local dir_output_file
        if [[ -n "$OUTPUT_FILE" ]]; then
            # User specified a custom output file - use it for all directories
            dir_output_file="$OUTPUT_FILE"
        else
            # Auto-generate filename: <dir_name>_statistics.<ext>
            local dir_name=$(echo "$input_dir" | tr '/' '_')  # Replace slashes with underscores
            dir_output_file="${dataset_path}/${dir_name}_statistics.${file_ext}"
        fi

        # Check if output file exists
        if [[ -f "$dir_output_file" ]] && [[ "$FORCE_OVERWRITE" == "false" ]]; then
            log_warning "Output file already exists: $dir_output_file (skipping directory)"
            log_warning "Use --force to overwrite"
            continue
        fi

        log_info "Processing directory: $input_dir"
        log_info "Output file: $dir_output_file"
        echo ""

        # Find EDS/l-EDS files
        local eds_files
        eds_files=("$input_path"/$FILE_PATTERN.eds "$input_path"/$FILE_PATTERN.leds)
        local found_files
        found_files=()

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

        # Create/truncate output file
        > "$dir_output_file"

        # Print CSV header if needed
        if [[ "$OUTPUT_FORMAT" == "csv" ]]; then
            print_csv_header > "$dir_output_file"
        fi

        # Export variables needed for parallel execution
        export STATS_TOOL OUTPUT_FORMAT FULL_MODE

        # Process files in parallel for json/csv, sequentially for table
        if [[ "$OUTPUT_FORMAT" == "table" ]] && [[ $THREADS -gt 1 ]]; then
            log_warning "Table format does not support parallel execution. Processing sequentially."
        fi

        if [[ "$OUTPUT_FORMAT" != "table" ]] && [[ $THREADS -gt 1 ]]; then
            # Parallel for json/csv
            printf "%s\n" "${found_files[@]}" | xargs -P "$THREADS" -I {} \
                bash -c "generate_stats \"{}\" \"$input_dir\"" >> "$dir_output_file"
        else
            # Sequential for table or threads=1
            for eds_file in "${found_files[@]}"; do
                generate_stats "$eds_file" "$input_dir" >> "$dir_output_file"
            done
        fi

        log_success "Saved statistics to: $dir_output_file"
        OUTPUT_FILES+=("$dir_output_file")
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
        --threads)
            THREADS="$2"
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
