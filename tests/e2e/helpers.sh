#!/bin/bash
# Shared utilities for EDSParser e2e tests.

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

YELLOW='\033[0;33m'

TESTS_PASSED=0
TESTS_FAILED=0
TESTS_SKIPPED=0

# Return code a test function uses to say "skipped" (the automake/ctest
# convention). run_test counts it separately; a skip is never a pass.
SKIP_RC=77

# Report a skip from inside a test function:  ... || { skip "reason"; return; }
# (`return` with no argument returns skip's status, i.e. $SKIP_RC.)
skip() {
    echo -e "  ${YELLOW}SKIP${NC}: $*"
    return $SKIP_RC
}

# Find a tool binary: the build tree only, unless PATH is asked for explicitly.
#
# The order matters and used to be the other way round. An installed binary in
# ~/.local/bin shadowed the build, so the suite silently tested whatever was
# last installed — on 2026-08-11 that was a Jul 6 eds2leds, which reported 10
# failures for flags the fresh build has. A test suite that does not test the
# tree it was run from is worse than no test suite. It also used to fall back to
# PATH when the build tree lacked a tool, which is the same failure one step
# removed, so that fallback is gone (2026-10-01).
#
#   EDSPARSER_TOOLS_DIR=<dir>     build tree's tools/ (default $EDSPARSER_ROOT/build/tools)
#   EDSPARSER_TOOLS_FROM_PATH=1   test installed binaries from PATH instead, deliberately
#
# Prints the path on stdout; diagnostics go to stderr.
find_tool() {
    local name="$1"
    if [ "${EDSPARSER_TOOLS_FROM_PATH:-0}" = "1" ]; then
        if command -v "$name" &>/dev/null; then
            command -v "$name"
            return 0
        fi
        echo -e "${RED}ERROR${NC}: $name not on PATH (EDSPARSER_TOOLS_FROM_PATH=1)" >&2
        return 1
    fi
    local build_path="${EDSPARSER_TOOLS_DIR:-$EDSPARSER_ROOT/build/tools}/$name"
    if [ -x "$build_path" ]; then
        echo "$build_path"
        return 0
    fi
    echo -e "${RED}ERROR${NC}: $name not built: $build_path" >&2
    echo "       Build it, or set EDSPARSER_TOOLS_FROM_PATH=1 to test an installed one." >&2
    return 1
}

# Check that a binary was built from the tree under test.
#
# Every tool prints COMMIT=<sha7> / DIRTY=<0|1> under --version (cmake/
# GitVersion.cmake). The binary is stale if its commit is not HEAD, if its DIRTY
# flag disagrees with the work tree, or — when both are dirty, which the flag
# alone cannot tell apart — if any modified tracked file is newer than it.
# Returns 0 when it matches, 1 (with the reason on stderr) when it does not.
check_tool_provenance() {
    local path="$1" ver commit dirty head tree_dirty=0 newer=""
    ver=$("$path" --version 2>&1)
    commit=$(echo "$ver" | sed -n 's/^COMMIT=//p' | head -1)
    dirty=$(echo "$ver" | sed -n 's/^DIRTY=//p' | head -1)
    echo "  using $path" >&2
    echo "$ver" | sed 's/^/    /' >&2

    if ! head=$(git -C "$EDSPARSER_ROOT" rev-parse HEAD 2>/dev/null); then
        echo -e "  ${YELLOW}WARN${NC}: $EDSPARSER_ROOT is not a git work tree; provenance not checked" >&2
        return 0
    fi
    [ -n "$(git -C "$EDSPARSER_ROOT" status --porcelain --untracked-files=no 2>/dev/null)" ] && tree_dirty=1

    if [ -z "$commit" ] || [ "$commit" = "unknown" ]; then
        echo -e "  ${RED}STALE${NC}: $path reports no COMMIT — cannot tell what it was built from" >&2
        return 1
    fi
    if [[ "$head" != "$commit"* ]]; then
        echo -e "  ${RED}STALE${NC}: $path built from $commit, work tree is at ${head:0:7}" >&2
        return 1
    fi
    if [ "${dirty:-?}" != "$tree_dirty" ]; then
        echo -e "  ${RED}STALE${NC}: $path has DIRTY=${dirty:-?}, work tree has DIRTY=$tree_dirty" >&2
        return 1
    fi
    if [ "$tree_dirty" = "1" ]; then
        local f
        while IFS= read -r f; do
            [ -n "$f" ] && [ "$EDSPARSER_ROOT/$f" -nt "$path" ] && newer="$newer $f"
        done < <(git -C "$EDSPARSER_ROOT" diff --name-only HEAD 2>/dev/null)
        if [ -n "$newer" ]; then
            echo -e "  ${RED}STALE${NC}: modified since $path was built:$newer" >&2
            return 1
        fi
    fi
    return 0
}

# Resolve a tool into a variable, at the top of a suite (not inside run_test,
# whose stderr is discarded):
#   resolve_tool TOOL eds2leds              the suite's subject
#   resolve_tool GEN genrandomeds optional  needed by some tests only
#
# Missing or stale binaries end the suite with exit 1. Opt-outs, both loud:
#   EDSPARSER_ALLOW_STALE_TOOLS=1     a provenance mismatch is a warning
#   EDSPARSER_ALLOW_MISSING_TOOLS=1   a missing *optional* tool leaves VAR
#                                     empty; tests needing it skip (need_tool)
resolve_tool() {
    local __var="$1" name="$2" kind="${3:-required}" path
    if ! path=$(find_tool "$name"); then
        if [ "$kind" = "optional" ] && [ "${EDSPARSER_ALLOW_MISSING_TOOLS:-0}" = "1" ]; then
            echo -e "  ${YELLOW}WARN${NC}: $name missing; tests needing it will be SKIPPED (EDSPARSER_ALLOW_MISSING_TOOLS=1)" >&2
            printf -v "$__var" '%s' ""
            return 0
        fi
        exit 1
    fi
    if ! check_tool_provenance "$path"; then
        if [ "${EDSPARSER_ALLOW_STALE_TOOLS:-0}" = "1" ]; then
            echo -e "  ${YELLOW}WARN${NC}: testing a binary that does not match the work tree (EDSPARSER_ALLOW_STALE_TOOLS=1)" >&2
        else
            echo "       Rebuild, or set EDSPARSER_ALLOW_STALE_TOOLS=1 to test it anyway." >&2
            exit 1
        fi
    fi
    printf -v "$__var" '%s' "$path"
}

# Inside a test: skip when an optional tool was allowed to be missing.
#   need_tool "$GEN" genrandomeds || return
need_tool() {
    [ -n "$1" ] || skip "$2 not available (EDSPARSER_ALLOW_MISSING_TOOLS=1)"
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

# Run one test function; track pass/fail/skip. A function returning $SKIP_RC
# is a skip — counted and reported as one, never as a pass.
run_test() {
    local name="$1" func="$2" rc
    "$func" 2>/dev/null
    rc=$?
    if [ $rc -eq 0 ]; then
        echo -e "${GREEN}PASS${NC}: $name"
        TESTS_PASSED=$((TESTS_PASSED + 1))
    elif [ $rc -eq $SKIP_RC ]; then
        echo -e "${YELLOW}SKIP${NC}: $name"
        TESTS_SKIPPED=$((TESTS_SKIPPED + 1))
    else
        echo -e "${RED}FAIL${NC}: $name"
        TESTS_FAILED=$((TESTS_FAILED + 1))
    fi
}

# Print the suite's tally; exit status is failure iff anything failed. When
# EDSPARSER_E2E_TALLY names a file, "passed failed skipped" is appended to it
# so run_all.sh can total skips across suites.
print_summary() {
    local total=$((TESTS_PASSED + TESTS_FAILED + TESTS_SKIPPED))
    echo ""
    echo "  $TESTS_PASSED/$total passed, $TESTS_FAILED failed, $TESTS_SKIPPED skipped"
    [ -n "${EDSPARSER_E2E_TALLY:-}" ] && \
        echo "$TESTS_PASSED $TESTS_FAILED $TESTS_SKIPPED" >> "$EDSPARSER_E2E_TALLY"
    [ $TESTS_FAILED -eq 0 ]
}
