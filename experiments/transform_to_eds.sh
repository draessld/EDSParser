#!/bin/bash

# Universal EDSParser Experiment Script
# Supports MSA, VCF, and EDS input formats

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default configuration
DATASET_NAME=""
INPUT_FORMAT=""
INPUT_DIR=""
LENGTH_VALUES=(3 5 10 15 20)
FILE_PATTERN="*"
FORCE_OVERWRITE=false
GENERATE_STATISTICS=true
REFERENCE_FASTA=""  # Required for VCF input

# Tool paths
MSA2EDS_TOOL="msa2eds"
VCF2EDS_TOOL="vcf2eds"
EDS2LEDS_TOOL="eds2leds"

# Statistics tracking
STATS_FILE=""
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
Usage: $0 --dataset DATASET --format FORMAT [OPTIONS]

Universal experiment runner for MSA, VCF, and EDS inputs.

REQUIRED ARGUMENTS:
  --dataset DATASET   Dataset name (e.g., "SARS_cov2")
  --format FORMAT     Input format: "msa", "vcf", or "eds"

OPTIONS:
  --input-dir DIR     Input directory name (default: same as format)
  --pattern PATTERN   File pattern to process (default: "*")
  --lengths L1,L2,... Length values for l-EDS (default: "3,5,10,15,20")
  --reference FILE    Reference FASTA for VCF (auto-detected if <vcf_name>.{fasta,fa,fna} exists)
  --force             Overwrite existing output files
  --no-stats          Don't generate statistics.csv
  -h, --help          Show this help message

EXAMPLES:
  # MSA experiment (SARS-CoV-2)
  $0 --dataset SARS_cov2 --format msa

  # VCF experiment (auto-detects reference if chr21.fasta exists)
  $0 --dataset human_data --format vcf

  # VCF with explicit reference
  $0 --dataset human_chr1 --format vcf --reference genome.fasta

  # VCF with custom lengths and pattern
  $0 --dataset vcf_data --format vcf --pattern "chr*.vcf" --lengths 5,10,15

  # EDS to l-EDS transformation
  $0 --dataset precomputed --format eds

  # Custom length values
  $0 --dataset SARS_cov2 --format msa --lengths 2,4,8,16

  # Process specific files
  $0 --dataset SARS_cov2 --format msa --pattern "20*"

DIRECTORY STRUCTURE:
  datasets/DATASET_NAME/
    ├── INPUT_DIR/          # Input files (msa/, vcf/, eds/, etc.)
    ├── eds/                # EDS outputs (if converting from MSA/VCF)
    ├── 3_leds/             # l-EDS outputs for l=3
    ├── 5_leds/             # l-EDS outputs for l=5
    └── statistics.csv      # Metrics

EOF
}

detect_input_format() {
    local dataset_path="$1"

    # Try to auto-detect input format
    if [[ -d "$dataset_path/msa" ]]; then
        echo "msa"
    elif [[ -d "$dataset_path/vcf" ]]; then
        echo "vcf"
    elif [[ -d "$dataset_path/eds" ]]; then
        echo "eds"
    else
        echo ""
    fi
}

check_tools() {
    log_info "Checking for required tools..."

    case "$INPUT_FORMAT" in
        msa)
            if ! command -v "$MSA2EDS_TOOL" &> /dev/null; then
                log_error "Tool '$MSA2EDS_TOOL' not found"
                exit 1
            fi
            log_success "Found $MSA2EDS_TOOL: $(which $MSA2EDS_TOOL)"
            ;;
        vcf)
            if ! command -v "$VCF2EDS_TOOL" &> /dev/null; then
                log_error "Tool '$VCF2EDS_TOOL' not found"
                exit 1
            fi
            log_success "Found $VCF2EDS_TOOL: $(which $VCF2EDS_TOOL)"

            if [[ -z "$REFERENCE_FASTA" ]]; then
                log_error "VCF format requires --reference FASTA file"
                exit 1
            fi
            if [[ ! -f "$REFERENCE_FASTA" ]]; then
                log_error "Reference FASTA not found: $REFERENCE_FASTA"
                exit 1
            fi
            ;;
        eds)
            if ! command -v "$EDS2LEDS_TOOL" &> /dev/null; then
                log_error "Tool '$EDS2LEDS_TOOL' not found"
                exit 1
            fi
            log_success "Found $EDS2LEDS_TOOL: $(which $EDS2LEDS_TOOL)"
            ;;
    esac
}

create_directories() {
    local dataset_path="$1"

    log_info "Creating output directories..."

    if [[ "$INPUT_FORMAT" != "eds" ]]; then
        mkdir -p "$dataset_path/eds"
    fi

    for l in "${LENGTH_VALUES[@]}"; do
        mkdir -p "$dataset_path/${l}_leds"
    done

    log_success "Output directories created"
}

get_file_size() {
    local file="$1"
    if [[ -f "$file" ]]; then
        stat -c%s "$file" 2>/dev/null || stat -f%z "$file" 2>/dev/null || echo "0"
    else
        echo "0"
    fi
}

transform_to_eds() {
    local input_file="$1"
    local dataset_path="$2"
    local basename=$(basename "$input_file" | sed -E 's/\.(msa|vcf|fasta)$//')
    local eds_output="$dataset_path/eds/${basename}.eds"
    local seds_output="$dataset_path/eds/${basename}.seds"
    local log_output="$dataset_path/eds/${basename}.eds.log"

    # Skip if exists
    if [[ -f "$eds_output" ]] && [[ "$FORCE_OVERWRITE" == "false" ]]; then
        log_warning "Skipping $basename.eds (already exists)"
        return 0
    fi

    log_info "Transforming $basename → EDS..."

    local start_time=$SECONDS

    case "$INPUT_FORMAT" in
        msa)
            $MSA2EDS_TOOL \
                --input "$input_file" \
                --output "$eds_output" \
                --sources "$seds_output" \
                > "$log_output" 2>&1
            ;;
        vcf)
            # Auto-detect reference if not provided
            local reference_file="$REFERENCE_FASTA"
            if [[ -z "$reference_file" ]]; then
                local input_dir=$(dirname "$input_file")
                for ext in fasta fa fna; do
                    local candidate="${input_dir}/${basename}.${ext}"
                    if [[ -f "$candidate" ]]; then
                        reference_file="$candidate"
                        log_info "Auto-detected reference: $(basename $reference_file)"
                        break
                    fi
                done

                if [[ -z "$reference_file" ]]; then
                    log_error "No reference file found for $basename (tried: .fasta, .fa, .fna)"
                    return 1
                fi
            fi

            $VCF2EDS_TOOL \
                --input "$input_file" \
                --reference "$reference_file" \
                --output "$eds_output" \
                --sources "$seds_output" \
                > "$log_output" 2>&1
            ;;
    esac

    if [[ $? -eq 0 ]]; then
        local elapsed=$((SECONDS - start_time))
        local input_size=$(get_file_size "$input_file")
        local output_size=$(get_file_size "$eds_output")
        local seds_size=$(get_file_size "$seds_output")

        log_success "Created $basename.eds (${elapsed}s)"
        echo "$input_size,$output_size,$seds_size,$elapsed"
        return 0
    else
        log_error "Failed to transform $basename"
        cat "$log_output"
        return 1
    fi
}

transform_to_leds() {
    local eds_file="$1"
    local l_value="$2"
    local dataset_path="$3"
    local basename=$(basename "$eds_file" .eds)
    local leds_dir="$dataset_path/${l_value}_leds"
    local leds_output="$leds_dir/${basename}.leds"
    local seds_input="$dataset_path/eds/${basename}.seds"
    local seds_output="$leds_dir/${basename}.seds"
    local log_output="$leds_dir/${basename}.leds.log"

    # Skip if exists
    if [[ -f "$leds_output" ]] && [[ "$FORCE_OVERWRITE" == "false" ]]; then
        log_warning "Skipping $basename.leds (l=$l_value, already exists)"
        return 0
    fi

    log_info "Transforming $basename → l-EDS (l=$l_value)..."

    local start_time=$SECONDS

    # Use eds2leds for EDS → l-EDS transformation
    # Method is auto-detected: linear (with sources) or cartesian (without sources)
    if [[ -f "$seds_input" ]]; then
        $EDS2LEDS_TOOL \
            --input "$eds_file" \
            --sources "$seds_input" \
            --output "$leds_output" \
            --context-length "$l_value" \
            > "$log_output" 2>&1
    else
        $EDS2LEDS_TOOL \
            --input "$eds_file" \
            --output "$leds_output" \
            --context-length "$l_value" \
            > "$log_output" 2>&1
    fi

    if [[ $? -eq 0 ]]; then
        local elapsed=$((SECONDS - start_time))
        local output_size=$(get_file_size "$leds_output")

        # eds2leds auto-generates .seds with same basename as output
        local auto_seds="${leds_output%.leds}.seds"
        local seds_size=$(get_file_size "$auto_seds")

        # Copy to expected location if needed
        if [[ -f "$auto_seds" ]] && [[ "$auto_seds" != "$seds_output" ]]; then
            cp "$auto_seds" "$seds_output" 2>/dev/null
        fi

        log_success "Created $basename.leds (l=$l_value, ${elapsed}s)"
        echo "$output_size,$seds_size,$elapsed"
        return 0
    else
        log_error "Failed to transform $basename to l-EDS (l=$l_value)"
        cat "$log_output"
        return 1
    fi
}

process_file() {
    local input_file="$1"
    local dataset_path="$2"
    local basename=$(basename "$input_file" | sed -E 's/\.(msa|vcf|eds|fasta)$//')

    echo ""
    log_info "================================================"
    log_info "Processing: $basename"
    log_info "================================================"

    local input_size=$(get_file_size "$input_file")
    local stats_row="$basename,$input_size"

    # Transform to EDS (if not already EDS input)
    if [[ "$INPUT_FORMAT" != "eds" ]]; then
        if eds_stats=$(transform_to_eds "$input_file" "$dataset_path"); then
            stats_row="${stats_row},${eds_stats}"
        else
            stats_row="${stats_row},0,0,0,0"
            FAILED_FILES+=("$basename (EDS)")
            ((FAILURE_COUNT++))
            return
        fi
    else
        # Already EDS, just use the input file
        eds_file="$input_file"
        stats_row="${stats_row},$input_size,0,0"
    fi

    # Get EDS file path
    if [[ "$INPUT_FORMAT" == "eds" ]]; then
        local eds_file="$input_file"
    else
        local eds_file="$dataset_path/eds/${basename}.eds"
    fi

    # Transform to l-EDS for each length value
    for l in "${LENGTH_VALUES[@]}"; do
        if leds_stats=$(transform_to_leds "$eds_file" "$l" "$dataset_path"); then
            stats_row="${stats_row},${leds_stats}"
        else
            stats_row="${stats_row},0,0,0"
            FAILED_FILES+=("$basename (l-EDS l=$l)")
            ((FAILURE_COUNT++))
        fi
    done

    # Write statistics
    if [[ "$GENERATE_STATISTICS" == "true" ]]; then
        echo "$stats_row" >> "$STATS_FILE"
    fi

    ((SUCCESS_COUNT++))
}

initialize_statistics() {
    if [[ "$GENERATE_STATISTICS" != "true" ]]; then
        return
    fi

    log_info "Initializing statistics file: $STATS_FILE"

    local header="variant,input_size_bytes"

    if [[ "$INPUT_FORMAT" != "eds" ]]; then
        header="${header},eds_size_bytes,seds_size_bytes,eds_time_sec"
    else
        header="${header},eds_size_bytes,seds_size_bytes,eds_time_sec"
    fi

    for l in "${LENGTH_VALUES[@]}"; do
        header="${header},l${l}_size_bytes,l${l}_seds_size_bytes,l${l}_time_sec"
    done

    echo "$header" > "$STATS_FILE"
}

print_summary() {
    echo ""
    log_info "================================================"
    log_info "EXPERIMENT SUMMARY"
    log_info "================================================"
    log_success "Successfully processed: $SUCCESS_COUNT files"

    if [[ $FAILURE_COUNT -gt 0 ]]; then
        log_error "Failed transformations: $FAILURE_COUNT"
        for failed in "${FAILED_FILES[@]}"; do
            log_error "  - $failed"
        done
    fi

    if [[ "$GENERATE_STATISTICS" == "true" ]]; then
        log_info "Statistics saved to: $STATS_FILE"
    fi

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

    # Auto-detect or validate format
    if [[ -z "$INPUT_FORMAT" ]]; then
        INPUT_FORMAT=$(detect_input_format "$dataset_path")
        if [[ -z "$INPUT_FORMAT" ]]; then
            log_error "Could not auto-detect input format. Use --format"
            exit 1
        fi
        log_info "Auto-detected format: $INPUT_FORMAT"
    fi

    # Set input directory
    if [[ -z "$INPUT_DIR" ]]; then
        INPUT_DIR="$INPUT_FORMAT"
    fi

    local input_path="$dataset_path/$INPUT_DIR"

    if [[ ! -d "$input_path" ]]; then
        log_error "Input directory not found: $input_path"
        exit 1
    fi

    # Set statistics file
    STATS_FILE="$dataset_path/statistics.csv"

    log_info "Starting experiment"
    log_info "Dataset: $DATASET_NAME"
    log_info "Format: $INPUT_FORMAT"
    log_info "Length values: ${LENGTH_VALUES[*]}"
    log_info "File pattern: $FILE_PATTERN"

    # Checks and setup
    check_tools
    create_directories "$dataset_path"

    if [[ "$GENERATE_STATISTICS" == "true" ]]; then
        initialize_statistics
    fi

    # Find and process files
    local extension=""
    case "$INPUT_FORMAT" in
        msa) extension="msa" ;;
        vcf) extension="vcf" ;;
        eds) extension="eds" ;;
    esac

    local input_files=("$input_path"/$FILE_PATTERN.$extension)
    local total_files=${#input_files[@]}

    if [[ $total_files -eq 0 ]] || [[ ! -f "${input_files[0]}" ]]; then
        log_error "No files found matching: $FILE_PATTERN.$extension"
        exit 1
    fi

    log_info "Found $total_files file(s) to process"

    for input_file in "${input_files[@]}"; do
        if [[ -f "$input_file" ]]; then
            process_file "$input_file" "$dataset_path"
        fi
    done

    print_summary
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --dataset)
            DATASET_NAME="$2"
            shift 2
            ;;
        --format)
            INPUT_FORMAT="$2"
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
        --lengths)
            IFS=',' read -ra LENGTH_VALUES <<< "$2"
            shift 2
            ;;
        --reference)
            REFERENCE_FASTA="$2"
            shift 2
            ;;
        --force)
            FORCE_OVERWRITE=true
            shift
            ;;
        --no-stats)
            GENERATE_STATISTICS=false
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
