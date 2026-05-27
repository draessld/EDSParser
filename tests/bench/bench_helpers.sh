#!/bin/bash
# Shared utilities for EDSParser benchmarks.

# Find a tool binary: checks PATH first, then edsparser build/tools/.
find_tool() {
    local name="$1"
    if command -v "$name" &>/dev/null; then
        command -v "$name"
        return 0
    fi
    local build_path="$EDSPARSER_ROOT/build/tools/$name"
    if [ -x "$build_path" ]; then
        echo "$build_path"
        return 0
    fi
    return 1
}

# Run a command N times, capturing the [Performance] line from stderr each time.
# Sets globals: BENCH_RUNTIME_S  BENCH_PEAK_MEMORY_MB  (median of N runs)
# Usage: run_bench_scenario N cmd [args...]
run_bench_scenario() {
    local n="$1"; shift
    local runtimes=() memories=()
    for (( i=0; i<n; i++ )); do
        local perf_line
        # 2>&1 >/dev/null: stderr→pipe, stdout→/dev/null
        # || true: prevent grep exit-1 (no match) from triggering pipefail
        perf_line=$("$@" 2>&1 >/dev/null | grep '^\[Performance\]' || true)
        runtimes+=( "$(echo "$perf_line" | sed -n 's/.*Runtime: \([0-9.]*\)s.*/\1/p')" )
        memories+=( "$(echo "$perf_line" | sed -n 's/.*Peak Memory: \([0-9.]*\) MB.*/\1/p')" )
    done
    # Median: awk NR==2 works for N=3; fallback to first element for N=1
    BENCH_RUNTIME_S=$(printf '%s\n' "${runtimes[@]}" | sort -n | awk 'NR==2{print}')
    BENCH_RUNTIME_S="${BENCH_RUNTIME_S:-${runtimes[0]}}"
    BENCH_PEAK_MEMORY_MB=$(printf '%s\n' "${memories[@]}" | sort -n | awk 'NR==2{print}')
    BENCH_PEAK_MEMORY_MB="${BENCH_PEAK_MEMORY_MB:-${memories[0]}}"
}

# Return actual on-disk file size in MB (3 decimal places).
file_size_mb() {
    local path="$1"
    local bytes
    bytes=$(stat -c "%s" "$path")
    awk "BEGIN { printf \"%.3f\", $bytes / 1048576 }"
}

# Compute throughput: size_mb / runtime_s (3 decimal places).
compute_throughput() {
    local size_mb="$1" runtime_s="$2"
    awk "BEGIN { printf \"%.3f\", $size_mb / $runtime_s }"
}

# Append one measurement row to CSV; writes header if file is new.
write_csv_row() {
    local csv_file="$1" timestamp="$2" preset="$3" scenario="$4" \
          tool="$5" input_size_mb="$6" runtime_s="$7" peak_memory_mb="$8" throughput="$9"
    if [ ! -f "$csv_file" ]; then
        echo "timestamp,preset,scenario,tool,input_size_mb,runtime_s,peak_memory_mb,throughput_mb_s" > "$csv_file"
    fi
    echo "${timestamp},${preset},${scenario},${tool},${input_size_mb},${runtime_s},${peak_memory_mb},${throughput}" >> "$csv_file"
}

bench_log()  { echo "[BENCH] $*"; }
bench_warn() { echo "[WARN]  $*" >&2; }
bench_err()  { echo "[ERROR] $*" >&2; }
