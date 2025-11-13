#!/bin/bash
# clean_data.sh - Clean generated files from data directory
# Keeps only source input files (original data)

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Script directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
DATA_DIR="$SCRIPT_DIR/data"

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}Data Directory Cleanup Script${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

# Check if data directory exists
if [ ! -d "$DATA_DIR" ]; then
    echo -e "${RED}Error: Data directory not found: $DATA_DIR${NC}"
    exit 1
fi

# Function to count files
count_files() {
    local dir=$1
    local pattern=$2
    if [ -d "$dir" ]; then
        find "$dir" -maxdepth 1 -type f -name "$pattern" 2>/dev/null | wc -l
    else
        echo "0"
    fi
}

# Track what will be deleted
echo -e "${YELLOW}Analyzing files to clean...${NC}"
echo ""

total_to_delete=0

# VCF directory: Keep only .vcf and .fa files, delete everything else
if [ -d "$DATA_DIR/vcf" ]; then
    echo -e "${BLUE}VCF directory:${NC}"
    echo "  Source files (keep): .vcf and .fa"
    echo "  Generated files (delete): .eds, .seds, .leds"

    vcf_eds=$(count_files "$DATA_DIR/vcf" "*.eds")
    vcf_seds=$(count_files "$DATA_DIR/vcf" "*.seds")
    vcf_leds=$(count_files "$DATA_DIR/vcf" "*.leds")

    echo "    - $vcf_eds .eds files"
    echo "    - $vcf_seds .seds files"
    echo "    - $vcf_leds .leds files"

    total_to_delete=$((total_to_delete + vcf_eds + vcf_seds + vcf_leds))
    echo ""
fi

# MSA directory: Keep only .msa files, delete everything else
if [ -d "$DATA_DIR/msa" ]; then
    echo -e "${BLUE}MSA directory:${NC}"
    echo "  Source files (keep): .msa"
    echo "  Generated files (delete): .eds, .seds, .leds"

    msa_eds=$(count_files "$DATA_DIR/msa" "*.eds")
    msa_seds=$(count_files "$DATA_DIR/msa" "*.seds")
    msa_leds=$(count_files "$DATA_DIR/msa" "*.leds")

    echo "    - $msa_eds .eds files"
    echo "    - $msa_seds .seds files"
    echo "    - $msa_leds .leds files"

    total_to_delete=$((total_to_delete + msa_eds + msa_seds + msa_leds))
    echo ""
fi

# EDS directory: Keep only .eds files, delete .leds, .seds, and .peds files
if [ -d "$DATA_DIR/eds" ]; then
    echo -e "${BLUE}EDS directory:${NC}"
    echo "  Source files (keep): .eds only"
    echo "  Generated files (delete): .leds, .seds, .peds"

    eds_leds=$(count_files "$DATA_DIR/eds" "*.leds")
    eds_seds=$(count_files "$DATA_DIR/eds" "*.seds")
    eds_peds=$(count_files "$DATA_DIR/eds" "*.peds")

    echo "    - $eds_leds .leds files"
    echo "    - $eds_seds .seds files"
    echo "    - $eds_peds .peds files"

    total_to_delete=$((total_to_delete + eds_leds + eds_seds + eds_peds))
    echo ""
fi

# Summary
echo -e "${YELLOW}Total files to delete: $total_to_delete${NC}"
echo ""

# Ask for confirmation
if [ "$total_to_delete" -eq 0 ]; then
    echo -e "${GREEN}No generated files found. Data directory is already clean.${NC}"
    exit 0
fi

read -p "Do you want to proceed with deletion? (y/N): " -n 1 -r
echo ""

if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo -e "${YELLOW}Cleanup cancelled.${NC}"
    exit 0
fi

echo ""
echo -e "${BLUE}Cleaning up generated files...${NC}"
echo ""

deleted_count=0

# Clean VCF directory
if [ -d "$DATA_DIR/vcf" ]; then
    echo -e "${BLUE}Cleaning vcf/ directory...${NC}"

    # Delete .eds files
    for file in "$DATA_DIR/vcf"/*.eds; do
        if [ -f "$file" ]; then
            echo "  Deleting: $(basename "$file")"
            rm "$file"
            deleted_count=$((deleted_count + 1))
        fi
    done

    # Delete .seds files
    for file in "$DATA_DIR/vcf"/*.seds; do
        if [ -f "$file" ]; then
            echo "  Deleting: $(basename "$file")"
            rm "$file"
            deleted_count=$((deleted_count + 1))
        fi
    done

    # Delete .leds files
    for file in "$DATA_DIR/vcf"/*.leds; do
        if [ -f "$file" ]; then
            echo "  Deleting: $(basename "$file")"
            rm "$file"
            deleted_count=$((deleted_count + 1))
        fi
    done

    echo ""
fi

# Clean MSA directory
if [ -d "$DATA_DIR/msa" ]; then
    echo -e "${BLUE}Cleaning msa/ directory...${NC}"

    # Delete .eds files
    for file in "$DATA_DIR/msa"/*.eds; do
        if [ -f "$file" ]; then
            echo "  Deleting: $(basename "$file")"
            rm "$file"
            deleted_count=$((deleted_count + 1))
        fi
    done

    # Delete .seds files
    for file in "$DATA_DIR/msa"/*.seds; do
        if [ -f "$file" ]; then
            echo "  Deleting: $(basename "$file")"
            rm "$file"
            deleted_count=$((deleted_count + 1))
        fi
    done

    # Delete .leds files
    for file in "$DATA_DIR/msa"/*.leds; do
        if [ -f "$file" ]; then
            echo "  Deleting: $(basename "$file")"
            rm "$file"
            deleted_count=$((deleted_count + 1))
        fi
    done

    echo ""
fi

# Clean EDS directory
if [ -d "$DATA_DIR/eds" ]; then
    echo -e "${BLUE}Cleaning eds/ directory...${NC}"

    # Delete .leds files
    for file in "$DATA_DIR/eds"/*.leds; do
        if [ -f "$file" ]; then
            echo "  Deleting: $(basename "$file")"
            rm "$file"
            deleted_count=$((deleted_count + 1))
        fi
    done

    # Delete .seds files
    for file in "$DATA_DIR/eds"/*.seds; do
        if [ -f "$file" ]; then
            echo "  Deleting: $(basename "$file")"
            rm "$file"
            deleted_count=$((deleted_count + 1))
        fi
    done

    # Delete .peds files
    for file in "$DATA_DIR/eds"/*.peds; do
        if [ -f "$file" ]; then
            echo "  Deleting: $(basename "$file")"
            rm "$file"
            deleted_count=$((deleted_count + 1))
        fi
    done

    echo ""
fi

# Final summary
echo -e "${BLUE}========================================${NC}"
echo -e "${GREEN}Cleanup complete!${NC}"
echo -e "${GREEN}Deleted $deleted_count files${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""
