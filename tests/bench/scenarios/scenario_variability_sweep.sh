#!/bin/bash
# Benchmark: eds2leds cartesian vs linear at increasing variant density.
# Fixed: ref_size, l=5, --min-context 5.  Varies: --variability.
# Shows how throughput degrades as the EDS becomes more degenerate.

run_scenario_variability_sweep() {
    local tmpdir="$1" csv="$2" ts="$3" preset="$4" n_reps="$5" ref_size_mb="$6"
    shift 6
    local variabilities=("$@")

    local gen_tool eds_tool
    gen_tool=$(find_tool genrandomeds) || { bench_err "genrandomeds not found"; return 1; }
    eds_tool=$(find_tool eds2leds)     || { bench_err "eds2leds not found"; return 1; }

    local seed=100
    for var in "${variabilities[@]}"; do
        local var_tag
        var_tag=$(echo "$var" | tr '.' '_')   # 0.05 → 0_05

        local input_eds="$tmpdir/var_${var_tag}_${ref_size_mb}mb.eds"
        local input_seds="$tmpdir/var_${var_tag}_${ref_size_mb}mb.seds"
        local out_cart="$tmpdir/var_${var_tag}_cart.leds"
        local out_lin="$tmpdir/var_${var_tag}_lin.leds"

        bench_log "generating input for variability=$var ..."
        "$gen_tool" --ref-size-mb "$ref_size_mb" --seed $((seed++)) \
            --variability "$var" --min-context 5 -o "$input_eds" 2>/dev/null

        local size_mb
        size_mb=$(file_size_mb "$input_eds")

        # Cartesian
        local scenario_cart="variability_${var_tag}_cartesian"
        bench_log "scenario=$scenario_cart  (N=$n_reps)"
        run_bench_scenario "$n_reps" \
            "$eds_tool" -i "$input_eds" -o "$out_cart" -l 5
        local tp_cart
        tp_cart=$(compute_throughput "$size_mb" "$BENCH_RUNTIME_S")
        write_csv_row "$csv" "$ts" "$preset" "$scenario_cart" \
            "eds2leds" "$size_mb" "$n_reps" "$tp_cart"
        bench_log "  [cartesian] median=${BENCH_RUNTIME_S}s  ±${BENCH_RUNTIME_STDDEV_S}s  p99=${BENCH_RUNTIME_P99_S}s  mem=${BENCH_PEAK_MEMORY_MB}MB  throughput=${tp_cart}MB/s"

        # Linear
        local scenario_lin="variability_${var_tag}_linear"
        bench_log "scenario=$scenario_lin  (N=$n_reps)"
        run_bench_scenario "$n_reps" \
            "$eds_tool" -i "$input_eds" -s "$input_seds" -o "$out_lin" -l 5
        local tp_lin
        tp_lin=$(compute_throughput "$size_mb" "$BENCH_RUNTIME_S")
        write_csv_row "$csv" "$ts" "$preset" "$scenario_lin" \
            "eds2leds" "$size_mb" "$n_reps" "$tp_lin"
        bench_log "  [linear]    median=${BENCH_RUNTIME_S}s  ±${BENCH_RUNTIME_STDDEV_S}s  p99=${BENCH_RUNTIME_P99_S}s  mem=${BENCH_PEAK_MEMORY_MB}MB  throughput=${tp_lin}MB/s"
    done
}
