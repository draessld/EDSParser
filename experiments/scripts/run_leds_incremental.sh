#!/bin/bash
# Stage 2 master (incremental): same orchestration as run_leds.sh — one screen per
# chromosome, memory cap, adoption of already-running workers, resumable — but each
# l-EDS is built from the largest already-computed smaller l-EDS instead of from the
# raw EDS (see run_chrom_leds_incremental.sh for the correctness argument).
#
# Use this when the smaller l-EDS files already exist and you only need to extend to
# larger l (e.g. l=100 from an existing l=50): it re-merges the small, already-partly
# -merged inputs rather than reprocessing the full EDS from scratch.
#
# Run inside a screen session, e.g.:  screen -S run_leds_inc
#
# Env: identical to run_leds.sh (L_VALUES, MAX_PARALLEL, MIN_FREE_GB, MEM_KILL_GB).
# Uses distinct screen names (run_leds_inc / eds2leds_inc_<chrom>) so it can coexist
# with a normal run_leds.sh run without clashing on screen sessions.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export MASTER_SCREEN="${MASTER_SCREEN:-run_leds_inc}"
export SCREEN_PREFIX="${SCREEN_PREFIX:-eds2leds_inc}"
export WORKER_SCRIPT="$SCRIPT_DIR/run_chrom_leds_incremental.sh"

exec bash "$SCRIPT_DIR/run_leds.sh" "$@"
