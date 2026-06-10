#!/bin/bash
# Benchmark: eds2leds memory stability with large inputs.
#
# Generates files large enough that a naive in-memory (FULL-mode) implementation
# would exceed a safe memory bound, then verifies that the METADATA_ONLY streaming
# architecture keeps peak RSS below that bound regardless of file size.
#
# THRESHOLD: 500 MB — the design ceiling of the METADATA_ONLY streaming architecture.
#   Expected peak for a 200 MB EDS: ~5 MB metadata + process overhead ≈ 50-100 MB.
#   A regression to FULL-mode loading of a 200 MB EDS would use 200 MB+ for the data
#   alone, and more during merge iteration when intermediates are accumulated in RAM.
#
# Parameters:
#   --variability 0.10  worst case: maximises number of degenerate sets (= metadata entries)
#   --min-context 4     just below l=5, forces one merge pass through the temp-file chain
#
# N is fixed at 3 — memory is deterministic; we care about peak, not timing variance.

run_scenario_memory_stability() {
    local tmpdir="$1" csv="$2" ts="$3" preset="$4" n_reps="$5"
    shift 5
    local sizes_mb=("$@")

    if [ "${#sizes_mb[@]}" -eq 0 ]; then return 0; fi

    local gen_tool eds_tool
    gen_tool=$(find_tool genrandomeds) || { bench_err "genrandomeds not found"; return 1; }
    eds_tool=$(find_tool eds2leds)     || { bench_err "eds2leds not found"; return 1; }

    local MEMORY_LIMIT_MB=500
    local mem_reps=3

    for ref_size_mb in "${sizes_mb[@]}"; do
        local input_eds="$tmpdir/memstab_${ref_size_mb}mb.eds"
        local input_seds="$tmpdir/memstab_${ref_size_mb}mb.seds"

        bench_log "generating input for memory_stability (${ref_size_mb} MB, 10% var, --min-context 4) ..."
        "$gen_tool" --ref-size-mb "$ref_size_mb" --seed 77 \
            --variability 0.10 --min-context 4 \
            -o "$input_eds" 2>/dev/null

        local size_mb
        size_mb=$(file_size_mb "$input_eds")

        _memstab_check_mode() {
            local peak="$1" label="$2"
            if awk "BEGIN { exit (${peak} > ${MEMORY_LIMIT_MB}) ? 0 : 1 }"; then
                bench_warn "  MEMORY REGRESSION [${label}]: ${peak} MB > ${MEMORY_LIMIT_MB} MB limit"
            else
                bench_log "  [OK] ${label}: ${peak} MB <= ${MEMORY_LIMIT_MB} MB"
            fi
        }

        # ── Cartesian ────────────────────────────────────────────────────────────
        local out_cart="$tmpdir/memstab_${ref_size_mb}mb_cart.leds"
        local scenario_cart="memory_stability_${ref_size_mb}mb_cartesian"
        bench_log "scenario=$scenario_cart  (N=$mem_reps)"
        run_bench_scenario "$mem_reps" \
            "$eds_tool" -i "$input_eds" -o "$out_cart" -l 5

        write_csv_row "$csv" "$ts" "$preset" "$scenario_cart" \
            "eds2leds" "$size_mb" "$mem_reps" \
            "$(compute_throughput "$size_mb" "$BENCH_RUNTIME_S")"
        bench_log "  [cartesian] median=${BENCH_RUNTIME_S}s  mem=${BENCH_PEAK_MEMORY_MB}MB"
        _memstab_check_mode "$BENCH_PEAK_MEMORY_MB" "cartesian"

        # ── Linear ───────────────────────────────────────────────────────────────
        local out_lin="$tmpdir/memstab_${ref_size_mb}mb_lin.leds"
        local scenario_lin="memory_stability_${ref_size_mb}mb_linear"
        bench_log "scenario=$scenario_lin  (N=$mem_reps)"
        run_bench_scenario "$mem_reps" \
            "$eds_tool" -i "$input_eds" -s "$input_seds" -o "$out_lin" -l 5

        write_csv_row "$csv" "$ts" "$preset" "$scenario_lin" \
            "eds2leds" "$size_mb" "$mem_reps" \
            "$(compute_throughput "$size_mb" "$BENCH_RUNTIME_S")"
        bench_log "  [linear]    median=${BENCH_RUNTIME_S}s  mem=${BENCH_PEAK_MEMORY_MB}MB"
        _memstab_check_mode "$BENCH_PEAK_MEMORY_MB" "linear"
    done
}
