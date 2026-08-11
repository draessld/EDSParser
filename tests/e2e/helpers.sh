#!/bin/bash
# Shared utilities for EDSParser e2e tests.

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

TESTS_PASSED=0
TESTS_FAILED=0

# Find a tool binary: checks build/tools/ FIRST, then PATH.
#
# The order matters and used to be the other way round. An installed binary in
# ~/.local/bin shadowed the build, so the suite silently tested whatever was
# last installed — on 2026-08-11 that was a Jul 6 eds2leds, which reported 10
# failures for flags the fresh build has. A test suite that does not test the
# tree it was run from is worse than no test suite.
#
# Set EDSPARSER_TOOLS_FROM_PATH=1 to test installed binaries deliberately.
find_tool() {
    local name="$1"
    if [ "${EDSPARSER_TOOLS_FROM_PATH:-0}" != "1" ]; then
        local build_path="$EDSPARSER_ROOT/build/tools/$name"
        if [ -x "$build_path" ]; then
            echo "$build_path"
            return 0
        fi
    fi
    if command -v "$name" &>/dev/null; then
        command -v "$name"
        return 0
    fi
    return 1
}

assert_exit_code() {
    local expected="$1" actual="$2" msg="$3"
    if [ "$actual" -ne "$expected" ]; then
        echo -e "  ${RED}FAIL${NC}: $msg — expected exit $expected, got $actual"
        return 1
    fi
}

assert_file_exists() {
    local path="$1" msg="$2"
    if [ ! -f "$path" ]; then
        echo -e "  ${RED}FAIL${NC}: $msg — file not found: $path"
        return 1
    fi
}

assert_not_empty() {
    local path="$1" msg="$2"
    if [ ! -s "$path" ]; then
        echo -e "  ${RED}FAIL${NC}: $msg — file is empty: $path"
        return 1
    fi
}

assert_contains() {
    local text="$1" pattern="$2" msg="$3"
    if ! echo "$text" | grep -q "$pattern"; then
        echo -e "  ${RED}FAIL${NC}: $msg — pattern '$pattern' not found"
        return 1
    fi
}

assert_file_contains() {
    local path="$1" pattern="$2" msg="$3"
    if ! grep -q "$pattern" "$path"; then
        echo -e "  ${RED}FAIL${NC}: $msg — pattern '$pattern' not found in $path"
        return 1
    fi
}

assert_file_equal() {
    local actual="$1" expected="$2" msg="$3"
    if [ ! -f "$expected" ]; then
        echo -e "  ${RED}FAIL${NC}: $msg — expected file not found: $expected"
        return 1
    fi
    if ! diff -q "$actual" "$expected" >/dev/null 2>&1; then
        echo -e "  ${RED}FAIL${NC}: $msg — output differs from expected"
        diff "$actual" "$expected" | head -10 | sed 's/^/    /'
        return 1
    fi
}

assert_line_count() {
    local path="$1" expected="$2" msg="$3"
    local actual
    actual=$(wc -l < "$path")
    if [ "$actual" -ne "$expected" ]; then
        echo -e "  ${RED}FAIL${NC}: $msg — expected $expected lines, got $actual"
        return 1
    fi
}

# Run one test function; track pass/fail.
run_test() {
    local name="$1" func="$2"
    if "$func" 2>/dev/null; then
        echo -e "${GREEN}PASS${NC}: $name"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    else
        echo -e "${RED}FAIL${NC}: $name"
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
}

print_summary() {
    local total=$((TESTS_PASSED + TESTS_FAILED))
    echo ""
    echo "  $TESTS_PASSED/$total passed"
    [ $TESTS_FAILED -eq 0 ]
}
