#!/bin/bash
# UNINSTALL.sh - Uninstallation script for EDSParser

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}EDSParser Uninstallation Script${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

# Function to print status messages
print_status() {
    echo -e "${GREEN}[✓]${NC} $1"
}

print_error() {
    echo -e "${RED}[✗]${NC} $1"
}

print_info() {
    echo -e "${YELLOW}[i]${NC} $1"
}

# Remove tools
for tool in edsparser-transform edsparser-stats edsparser-genpatterns; do
    if [ -f "$HOME/.local/bin/$tool" ]; then
        rm -f "$HOME/.local/bin/$tool"
        print_status "Removed $tool from ~/.local/bin"
    fi
done

# Remove installed library and headers
if [ -f "$HOME/.local/lib/libedsparser_lib.a" ]; then
    rm -f "$HOME/.local/lib/libedsparser_lib.a"
    print_status "Removed library from ~/.local/lib"
else
    print_info "Library not found in ~/.local/lib"
fi

if [ -d "$HOME/.local/include/edsparser" ]; then
    rm -rf "$HOME/.local/include/edsparser"
    print_status "Removed headers from ~/.local/include"
else
    print_info "Headers not found in ~/.local/include"
fi

# Remove CMake config files
if [ -d "$HOME/.local/lib/cmake/EDSParser" ]; then
    rm -rf "$HOME/.local/lib/cmake/EDSParser"
    print_status "Removed CMake config files"
else
    print_info "CMake config files not found"
fi

# Remove build directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
if [ -d "$SCRIPT_DIR/build" ]; then
    rm -rf "$SCRIPT_DIR/build"
    print_status "Removed build directory"
else
    print_info "Build directory not found"
fi

echo ""
echo -e "${GREEN}Uninstallation completed successfully!${NC}"
echo ""
