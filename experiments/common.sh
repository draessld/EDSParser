#!/bin/bash
# common.sh - Shared utilities for experiment scripts

# Colors for consistent logging
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

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

# Function to resolve dataset path from name or relative/absolute path
resolve_dataset_path() {
    local input="$1"

    if [[ "$input" == /* ]]; then
        echo "$input" # Absolute path
    elif [[ "$input" == */* ]]; then
        # Convert relative path to absolute
        echo "$(cd "$(dirname "$input")" 2>/dev/null && pwd)/$(basename "$input")"
    else
        # Treat as dataset name, use datasets/ prefix from script's location
        echo "$(dirname "${BASH_SOURCE[0]}")/datasets/$input"
    fi
}