#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDSPARSER_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/bench_helpers.sh"
for s in "$SCRIPT_DIR/scenarios"/scenario_*.sh; do source "$s"; done

PRESET="standard"
SCENARIO="all"
RESULTS_DIR="$SCRIPT_DIR/results"
TMPDIR_BENCH="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_BENCH"' EXIT

VALID_SCENARIOS=(
    genrandomeds
    eds2leds_cartesian
    eds2leds_linear
    edsparser_stats
    edsparser_genpatterns
    variability_sweep
    context_length_sweep
    path_count_sweep
    memory_stability
    chain_merging
    edz_format
    all
)

show_help() {
    cat <<EOF
Usage: bench.sh [--size PRESET] [--scenario NAME]

  --size quick      N=5,  EDS 1-5 MB,    transform at 1+5 MB                     (~30 s)
  --size standard   N=30, EDS 1-10 MB,   transform at 1+5+10 MB                   (~5 min)  [default]
  --size large      N=30, EDS 5-50 MB,   transform at 5+10+20+50 MB               (~25 min)
  --size scaling    N=10, EDS 10 MB,     sweeps: var×5, ctx×5, paths×4, memstab×2 (~15 min)
                    Designed to show how runtime and memory grow with:
                      variability (0.01–0.40), context l (3–20), paths (2–16)
                    Uses fixed path counts (N:N) for clean 1-variable sweeps.

  --scenario NAME   run only the named scenario (default: all)
                    valid names:
                      genrandomeds         genrandomeds size sweep
                      eds2leds_cartesian   eds2leds cartesian size sweep
                      eds2leds_linear      eds2leds linear size sweep
                      edsparser_stats      edsparser-stats size sweep
                      edsparser_genpatterns edsparser-genpatterns size sweep
                      variability_sweep    eds2leds runtime vs variant density
                      context_length_sweep eds2leds runtime vs context length l
                      path_count_sweep     eds2leds runtime vs number of paths
                      memory_stability     peak RSS check on large files (limit=500 MB)
                      chain_merging        eds2leds chain-merging throughput vs variant density
                      edz_format           SEDS vs EDZ write speed and file-size ratio
                      all                  run everything (default)

All tools (eds2leds cartesian/linear, edsparser-genpatterns) are run across
the full SIZES_MB array so that the size-sweep plot shows trend lines, not
single points.

Sweep scenarios (variability, context length, path count) are included in
standard, large, and scaling presets but skipped in quick.

Memory stability scenario (large-file peak RSS check, limit=500 MB) is included
in standard (20+50 MB), large (50+100+200 MB), and scaling (50+100 MB) presets.

Results are written to tests/bench/results/YYYY-MM-DD_HH-MM-SS.csv.
Run bench_compare.sh afterwards to check for regressions.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --size)     PRESET="$2";   shift 2 ;;
        --scenario) SCENARIO="$2"; shift 2 ;;
        -h|--help)  show_help; exit 0 ;;
        *) bench_err "Unknown flag: $1"; exit 1 ;;
    esac
done

# Validate --scenario value
valid=0
for name in "${VALID_SCENARIOS[@]}"; do
    [[ "$SCENARIO" == "$name" ]] && valid=1 && break
done
if [[ $valid -eq 0 ]]; then
    bench_err "Unknown scenario: '$SCENARIO'"
    bench_err "Valid names: ${VALID_SCENARIOS[*]}"
    exit 1
fi

case "$PRESET" in
    quick)
        N_REPS=5
        SIZES_MB=(1 5)
        VAR_SWEEP_MB=0          # 0 = skip sweep scenarios
        CTX_SWEEP_MB=0
        PATH_SWEEP_MB=0
        MEM_STAB_SIZES=()       # skip — files too small to stress memory
        CHAIN_MERGING_MB=0      # skip in quick — merge path exercised by standard+
        CHAIN_MERGING_VAR_VALS=()
        EDZ_FORMAT_SIZES=(1)    # quick single-size check
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
        MEM_STAB_SIZES=(20 50)  # larger than SIZES_MB to stress METADATA_ONLY path
        CHAIN_MERGING_MB=5
        CHAIN_MERGING_VAR_VALS=(0.01 0.05)
        EDZ_FORMAT_SIZES=(1 5)
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
        MEM_STAB_SIZES=(50 100 200)
        CHAIN_MERGING_MB=10
        CHAIN_MERGING_VAR_VALS=(0.01 0.05 0.10)
        EDZ_FORMAT_SIZES=(5 10)
        ;;
    scaling)
        # Purpose: measure how runtime and memory grow with variability, context l, and
        # number of paths on a fixed 10 MB input. Uses fixed min=max path counts (N:N)
        # so each data point has exactly N alternatives per degenerate symbol.
        N_REPS=10
        SIZES_MB=(10)
        VAR_SWEEP_MB=10
        CTX_SWEEP_MB=10
        PATH_SWEEP_MB=10
        VAR_SWEEP_VALS=(0.01 0.05 0.10 0.20 0.40)
        CTX_SWEEP_VALS=(3 5 10 15 20)
        PATH_SWEEP_CFGS=(2:2 4:4 8:8 16:16)
        MEM_STAB_SIZES=(50 100)
        CHAIN_MERGING_MB=10
        CHAIN_MERGING_VAR_VALS=(0.01 0.05 0.10)
        EDZ_FORMAT_SIZES=(10)
        ;;
    *)
        bench_err "Unknown preset: $PRESET (use quick|standard|large|scaling)"
        exit 1
        ;;
esac

# Returns 0 (true) when the named scenario should run.
_should_run() { [[ "$SCENARIO" == "all" || "$SCENARIO" == "$1" ]]; }

# Warn when a sweep scenario is requested explicitly but the preset would skip it.
_check_sweep_available() {
    local name="$1" mb_var="$2"
    if [[ "$SCENARIO" == "$name" && "$mb_var" -eq 0 ]]; then
        bench_warn "Scenario '$name' is skipped for preset '$PRESET' (sweep size = 0)."
        bench_warn "Use --size standard or --size large to enable it."
        exit 1
    fi
}

_check_mem_stab_available() {
    if [[ "$SCENARIO" == "memory_stability" && "${#MEM_STAB_SIZES[@]}" -eq 0 ]]; then
        bench_warn "Scenario 'memory_stability' is skipped for preset '$PRESET' (no sizes defined)."
        bench_warn "Use --size standard or --size large to enable it."
        exit 1
    fi
}

# Eagerly validate that a specifically requested sweep scenario can actually run.
_check_sweep_available variability_sweep    "$VAR_SWEEP_MB"
_check_sweep_available context_length_sweep "$CTX_SWEEP_MB"
_check_sweep_available path_count_sweep     "$PATH_SWEEP_MB"
_check_sweep_available chain_merging        "${CHAIN_MERGING_MB:-0}"
_check_mem_stab_available

TIMESTAMP=$(date '+%Y-%m-%d_%H-%M-%S')
mkdir -p "$RESULTS_DIR"
CSV_FILE="$RESULTS_DIR/${TIMESTAMP}.csv"

bench_log "EDSParser benchmark — preset=$PRESET  scenario=$SCENARIO  N=$N_REPS"
bench_log "Results → $CSV_FILE"
echo ""

bench_check_environment
echo ""

if _should_run genrandomeds; then
    run_scenario_genrandomeds "$TMPDIR_BENCH" "$CSV_FILE" "$TIMESTAMP" "$PRESET" "$N_REPS" "${SIZES_MB[@]}"
    echo ""
fi

if _should_run eds2leds_cartesian; then
    run_scenario_eds2leds_cartesian "$TMPDIR_BENCH" "$CSV_FILE" "$TIMESTAMP" "$PRESET" "$N_REPS" "${SIZES_MB[@]}"
    echo ""
fi

if _should_run eds2leds_linear; then
    run_scenario_eds2leds_linear "$TMPDIR_BENCH" "$CSV_FILE" "$TIMESTAMP" "$PRESET" "$N_REPS" "${SIZES_MB[@]}"
    echo ""
fi

if _should_run edsparser_stats; then
    run_scenario_edsparser_stats "$TMPDIR_BENCH" "$CSV_FILE" "$TIMESTAMP" "$PRESET" "$N_REPS" "${SIZES_MB[@]}"
    echo ""
fi

if _should_run edsparser_genpatterns; then
    run_scenario_edsparser_genpatterns "$TMPDIR_BENCH" "$CSV_FILE" "$TIMESTAMP" "$PRESET" "$N_REPS" "${SIZES_MB[@]}"
    echo ""
fi

if _should_run variability_sweep && [ "$VAR_SWEEP_MB" -gt 0 ]; then
    bench_log "=== Sweep: variability (cartesian vs linear) ==="
    run_scenario_variability_sweep "$TMPDIR_BENCH" "$CSV_FILE" "$TIMESTAMP" "$PRESET" "$N_REPS" \
        "$VAR_SWEEP_MB" "${VAR_SWEEP_VALS[@]}"
    echo ""
fi

if _should_run context_length_sweep && [ "$CTX_SWEEP_MB" -gt 0 ]; then
    bench_log "=== Sweep: context length (cartesian vs linear) ==="
    run_scenario_context_length_sweep "$TMPDIR_BENCH" "$CSV_FILE" "$TIMESTAMP" "$PRESET" "$N_REPS" \
        "$CTX_SWEEP_MB" "${CTX_SWEEP_VALS[@]}"
    echo ""
fi

if _should_run path_count_sweep && [ "$PATH_SWEEP_MB" -gt 0 ]; then
    bench_log "=== Sweep: path count (linear only) ==="
    run_scenario_path_count_sweep "$TMPDIR_BENCH" "$CSV_FILE" "$TIMESTAMP" "$PRESET" "$N_REPS" \
        "$PATH_SWEEP_MB" "${PATH_SWEEP_CFGS[@]}"
    echo ""
fi

if _should_run memory_stability && [ "${#MEM_STAB_SIZES[@]}" -gt 0 ]; then
    bench_log "=== Memory stability: large-file peak RSS check (limit=500 MB) ==="
    run_scenario_memory_stability "$TMPDIR_BENCH" "$CSV_FILE" "$TIMESTAMP" "$PRESET" "$N_REPS" \
        "${MEM_STAB_SIZES[@]}"
    echo ""
fi

if _should_run chain_merging && [ "$CHAIN_MERGING_MB" -gt 0 ]; then
    bench_log "=== Chain-merging throughput vs variant density (--min-context 0) ==="
    run_scenario_chain_merging "$TMPDIR_BENCH" "$CSV_FILE" "$TIMESTAMP" "$PRESET" "$N_REPS" \
        "$CHAIN_MERGING_MB" "${CHAIN_MERGING_VAR_VALS[@]}"
    echo ""
fi

if _should_run edz_format && [ "${#EDZ_FORMAT_SIZES[@]}" -gt 0 ]; then
    bench_log "=== EDZ format: SEDS vs EDZ write speed + file-size ratio ==="
    run_scenario_edz_format "$TMPDIR_BENCH" "$CSV_FILE" "$TIMESTAMP" "$PRESET" "$N_REPS" \
        "${EDZ_FORMAT_SIZES[@]}"
    echo ""
fi

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
