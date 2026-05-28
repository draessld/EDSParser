#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDSPARSER_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/bench_helpers.sh"
for s in "$SCRIPT_DIR/scenarios"/scenario_*.sh; do source "$s"; done

PRESET="standard"
RESULTS_DIR="$SCRIPT_DIR/results"
TMPDIR_BENCH="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_BENCH"' EXIT

show_help() {
    cat <<EOF
Usage: bench.sh [--size PRESET]

  --size quick      N=1, EDS 1-5 MB,    transform at 1+5 MB        (~30 s)
  --size standard   N=3, EDS 1-10 MB,   transform at 1+5+10 MB     (~5 min)  [default]
  --size large      N=3, EDS 5-50 MB,   transform at 5+10+20+50 MB (~25 min)

All tools (eds2leds cartesian/linear, edsparser-genpatterns) are run across
the full SIZES_MB array so that the size-sweep plot shows trend lines, not
single points.

Sweep scenarios (variability, context length, path count) are included in
standard and large presets but skipped in quick.

Results are written to tests/bench/results/YYYY-MM-DD_HH-MM-SS.csv.
Run bench_compare.sh afterwards to check for regressions.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --size) PRESET="$2"; shift 2 ;;
        -h|--help) show_help; exit 0 ;;
        *) bench_err "Unknown flag: $1"; exit 1 ;;
    esac
done

case "$PRESET" in
    quick)
        N_REPS=5
        SIZES_MB=(1 5)
        VAR_SWEEP_MB=0          # 0 = skip sweep scenarios
        CTX_SWEEP_MB=0
        PATH_SWEEP_MB=0
        ;;
    standard)
        N_REPS=30
        SIZES_MB=(1 5 10)
        VAR_SWEEP_MB=5          # variability sweep input size
        CTX_SWEEP_MB=5          # context length sweep input size
        PATH_SWEEP_MB=5         # path count sweep input size
        VAR_SWEEP_VALS=(0.01 0.05 0.10)
        CTX_SWEEP_VALS=(3 5 10 20)
        PATH_SWEEP_CFGS=(2:2 2:4 2:8)
        ;;
    large)
        N_REPS=30
        SIZES_MB=(5 10 20 50)
        VAR_SWEEP_MB=10
        CTX_SWEEP_MB=10
        PATH_SWEEP_MB=10
        VAR_SWEEP_VALS=(0.01 0.05 0.10 0.30)
        CTX_SWEEP_VALS=(3 5 10 20)
        PATH_SWEEP_CFGS=(2:2 2:4 2:8 4:8)
        ;;
    *)
        bench_err "Unknown preset: $PRESET (use quick|standard|large)"
        exit 1
        ;;
esac

TIMESTAMP=$(date '+%Y-%m-%d_%H-%M-%S')
mkdir -p "$RESULTS_DIR"
CSV_FILE="$RESULTS_DIR/${TIMESTAMP}.csv"

bench_log "EDSParser benchmark — preset=$PRESET  N=$N_REPS"
bench_log "Results → $CSV_FILE"
echo ""

bench_check_environment
echo ""

run_scenario_genrandomeds          "$TMPDIR_BENCH" "$CSV_FILE" "$TIMESTAMP" "$PRESET" "$N_REPS" "${SIZES_MB[@]}"
echo ""
run_scenario_eds2leds_cartesian    "$TMPDIR_BENCH" "$CSV_FILE" "$TIMESTAMP" "$PRESET" "$N_REPS" "${SIZES_MB[@]}"
echo ""
run_scenario_eds2leds_linear       "$TMPDIR_BENCH" "$CSV_FILE" "$TIMESTAMP" "$PRESET" "$N_REPS" "${SIZES_MB[@]}"
echo ""
run_scenario_edsparser_stats       "$TMPDIR_BENCH" "$CSV_FILE" "$TIMESTAMP" "$PRESET" "$N_REPS" "${SIZES_MB[@]}"
echo ""
run_scenario_edsparser_genpatterns "$TMPDIR_BENCH" "$CSV_FILE" "$TIMESTAMP" "$PRESET" "$N_REPS" "${SIZES_MB[@]}"

if [ "$VAR_SWEEP_MB" -gt 0 ]; then
    echo ""
    bench_log "=== Sweep: variability (cartesian vs linear) ==="
    run_scenario_variability_sweep "$TMPDIR_BENCH" "$CSV_FILE" "$TIMESTAMP" "$PRESET" "$N_REPS" \
        "$VAR_SWEEP_MB" "${VAR_SWEEP_VALS[@]}"
fi

if [ "$CTX_SWEEP_MB" -gt 0 ]; then
    echo ""
    bench_log "=== Sweep: context length (cartesian vs linear) ==="
    run_scenario_context_length_sweep "$TMPDIR_BENCH" "$CSV_FILE" "$TIMESTAMP" "$PRESET" "$N_REPS" \
        "$CTX_SWEEP_MB" "${CTX_SWEEP_VALS[@]}"
fi

if [ "$PATH_SWEEP_MB" -gt 0 ]; then
    echo ""
    bench_log "=== Sweep: path count (linear only) ==="
    run_scenario_path_count_sweep "$TMPDIR_BENCH" "$CSV_FILE" "$TIMESTAMP" "$PRESET" "$N_REPS" \
        "$PATH_SWEEP_MB" "${PATH_SWEEP_CFGS[@]}"
fi

echo ""
bench_log "Done. Results written to: $CSV_FILE"
bench_log "Run bench_compare.sh to check for regressions."

echo ""
if python3 -c "import matplotlib, pandas" 2>/dev/null; then
    bench_log "Generating plots..."
    python3 "$SCRIPT_DIR/bench_plot.py" "$CSV_FILE" && \
        bench_log "Plots → $RESULTS_DIR/plots/$(basename "${CSV_FILE%.csv}")/"
else
    bench_log "Skipping plots — install Python dependencies to enable:"
    bench_log "  pip install -r tests/bench/requirements.txt"
    bench_log "  # or inside a venv: python3 -m venv .venv && source .venv/bin/activate && pip install -r tests/bench/requirements.txt"
fi
