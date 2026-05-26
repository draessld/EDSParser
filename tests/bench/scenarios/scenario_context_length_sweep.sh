#!/bin/bash
# Benchmark: eds2leds cartesian vs linear at increasing context length (l).
# Fixed: ref_size, variability=0.01 (low, no min-context → real merge work).
# Varies: l.  Higher l forces more merge iterations.

run_scenario_context_length_sweep() {
    local tmpdir="$1" csv="$2" ts="$3" preset="$4" n_reps="$5" ref_size_mb="$6"
    shift 6
    local lengths=("$@")

    local gen_tool eds_tool
    gen_tool=$(find_tool genrandomeds) || { bench_err "genrandomeds not found"; return 1; }
    eds_tool=$(find_tool eds2leds)     || { bench_err "eds2leds not found"; return 1; }

    # Generate once: no --min-context so adjacent degenerate symbols exist,
    # giving eds2leds real merging work at every l value.
    local input_eds="$tmpdir/ctx_input_${ref_size_mb}mb.eds"
    local input_seds="$tmpdir/ctx_input_${ref_size_mb}mb.seds"
    bench_log "generating input for context_length_sweep (ref=${ref_size_mb}MB, var=0.01) ..."
    "$gen_tool" --ref-size-mb "$ref_size_mb" --seed 200 \
        --variability 0.01 -o "$input_eds" 2>/dev/null

    local size_mb
    size_mb=$(file_size_mb "$input_eds")

    for l in "${lengths[@]}"; do
        local out_cart="$tmpdir/ctx_l${l}_cart.leds"
        local out_lin="$tmpdir/ctx_l${l}_lin.leds"

        # Cartesian
        local scenario_cart="context_l${l}_cartesian"
        bench_log "scenario=$scenario_cart  (N=$n_reps)"
        run_bench_scenario "$n_reps" \
            "$eds_tool" -i "$input_eds" -o "$out_cart" -l "$l"
        local tp_cart
        tp_cart=$(compute_throughput "$size_mb" "$BENCH_RUNTIME_S")
        write_csv_row "$csv" "$ts" "$preset" "$scenario_cart" \
            "eds2leds" "$size_mb" "$BENCH_RUNTIME_S" "$BENCH_PEAK_MEMORY_MB" "$tp_cart"
        bench_log "  [cartesian l=$l] runtime=${BENCH_RUNTIME_S}s  memory=${BENCH_PEAK_MEMORY_MB}MB  throughput=${tp_cart}MB/s"

        # Linear
        local scenario_lin="context_l${l}_linear"
        bench_log "scenario=$scenario_lin  (N=$n_reps)"
        run_bench_scenario "$n_reps" \
            "$eds_tool" -i "$input_eds" -s "$input_seds" -o "$out_lin" -l "$l"
        local tp_lin
        tp_lin=$(compute_throughput "$size_mb" "$BENCH_RUNTIME_S")
        write_csv_row "$csv" "$ts" "$preset" "$scenario_lin" \
            "eds2leds" "$size_mb" "$BENCH_RUNTIME_S" "$BENCH_PEAK_MEMORY_MB" "$tp_lin"
        bench_log "  [linear    l=$l] runtime=${BENCH_RUNTIME_S}s  memory=${BENCH_PEAK_MEMORY_MB}MB  throughput=${tp_lin}MB/s"
    done
}
