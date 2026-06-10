#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/bench_helpers.sh"

RESULTS_DIR="$SCRIPT_DIR/results"
BASELINE="$SCRIPT_DIR/baseline.csv"
THRESHOLD="1.20"

LATEST=$(ls -t "$RESULTS_DIR"/*.csv 2>/dev/null | head -1 || true)
if [ -z "$LATEST" ]; then
    bench_err "No result CSVs in $RESULTS_DIR — run bench.sh first."
    exit 1
fi

if [ ! -f "$BASELINE" ]; then
    bench_warn "No baseline.csv found — bootstrapping from: $LATEST"
    cp "$LATEST" "$BASELINE"
    bench_log "Baseline saved. Run bench.sh again, then re-run bench_compare.sh."
    exit 0
fi

bench_log "Comparing: $(basename "$LATEST")  vs  baseline.csv  (threshold: +20%)"
echo ""

awk -F',' -v threshold="$THRESHOLD" '
    FNR==1 {
        for (i=1; i<=NF; i++) col[$i] = i
        next
    }
    NR==FNR {
        key = $(col["scenario"])
        base_runtime[key] = $(col["runtime_median_s"])
        base_memory[key]  = $(col["memory_median_mb"])
        next
    }
    {
        scenario = $(col["scenario"])
        runtime  = $(col["runtime_median_s"])
        memory   = $(col["memory_median_mb"])
        if (scenario in base_runtime) {
            if ((runtime+0) > (base_runtime[scenario]+0) * threshold) {
                printf "REGRESSION runtime  %-44s  baseline=%6.3fs  current=%6.3fs  ratio=%.2fx\n",
                    scenario, base_runtime[scenario]+0, runtime+0,
                    (runtime+0) / (base_runtime[scenario]+0)
                regressions++
            }
            if ((memory+0) > (base_memory[scenario]+0) * threshold) {
                printf "REGRESSION memory   %-44s  baseline=%6.1fMB  current=%6.1fMB  ratio=%.2fx\n",
                    scenario, base_memory[scenario]+0, memory+0,
                    (memory+0) / (base_memory[scenario]+0)
                regressions++
            }
        } else {
            printf "NEW        %-44s  (no baseline)\n", scenario
        }
    }
    END {
        if (regressions > 0) {
            printf "\n%d regression(s) found.\n", regressions
            exit 1
        } else {
            print "All scenarios within threshold."
            exit 0
        }
    }
' "$BASELINE" "$LATEST"
