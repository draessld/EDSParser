#!/bin/bash
# Benchmark: eds2leds chain-merging throughput vs variant density.
#
# The [ARCH] fix (2026-06-28) replaced pairwise iteration with chain-aware group
# selection: a contiguous run of L positions that all need merging is now resolved
# in ONE full-file pass instead of L-1 passes.  To exercise this path, inputs must
# have adjacent degenerate symbols or short internal common blocks — violations that
# the existing eds2leds_cartesian/linear scenarios never produce because they use
# --min-context 5 with l=5 (already compliant, 0 iterations).
#
# This scenario uses --min-context 0 (Bernoulli mode) which can generate adjacent
# degenerate symbols.  Higher variability means more adjacent degenerates, longer
# chains, and more "merge work" per iteration — so this sweep reveals both the
# chain-merging correctness and throughput under real violation density.
#
# Fixed:  ref_size_mb (from caller), l=5, --min-context 0.
# Varies: --variability (0.01, 0.05, 0.10).
#   0.01 → rare violations, chains of length 1-2, ~1 iteration
#   0.05 → moderate violations, chains of 2-4, ~1 iteration
#   0.10 → frequent violations, longer chains, ~1 iteration
#
# Cartesian capped at variability 0.05 to avoid exponential alternative blowup.
# Linear tested at all values (output bounded by path count).
#
# The iteration count is captured outside the timing loop and logged alongside
# throughput so the single-pass resolution is directly visible.

run_scenario_chain_merging() {
    local tmpdir="$1" csv="$2" ts="$3" preset="$4" n_reps="$5" ref_size_mb="$6"
    shift 6
    local variabilities=("$@")

    local gen_tool eds_tool
    gen_tool=$(find_tool genrandomeds) || { bench_err "genrandomeds not found"; return 1; }
    eds_tool=$(find_tool eds2leds)     || { bench_err "eds2leds not found"; return 1; }

    local l=5
    local seed=400

    for var in "${variabilities[@]}"; do
        local var_tag
        var_tag=$(echo "$var" | tr '.' '_')   # 0.05 → 0_05

        local input_eds="$tmpdir/chain_v${var_tag}_${ref_size_mb}mb.eds"
        local input_seds="$tmpdir/chain_v${var_tag}_${ref_size_mb}mb.seds"
        local out_cart="$tmpdir/chain_v${var_tag}_cart.leds"
        local out_lin="$tmpdir/chain_v${var_tag}_lin.leds"

        bench_log "generating input for chain_merging (ref=${ref_size_mb}MB, var=${var}, min-context=0) ..."
        "$gen_tool" --ref-size-mb "$ref_size_mb" --seed $((seed++)) \
            --variability "$var" --min-context 0 -o "$input_eds" 2>/dev/null

        local size_mb
        size_mb=$(file_size_mb "$input_eds")

        # Run once outside the timing loop to capture iteration count.
        # Only run cartesian when variability <= 0.05 to avoid exponential blowup.
        local run_cart=1
        if awk "BEGIN { exit ($var > 0.05) ? 0 : 1 }"; then
            run_cart=0
            bench_log "  (cartesian skipped for var=$var — exponential blowup risk)"
        fi

        local iters_cart="skipped" iters_lin
        if [ "$run_cart" -eq 1 ]; then
            iters_cart=$("$eds_tool" -i "$input_eds" -o "$out_cart" -l "$l" 2>&1 >/dev/null \
                | grep 'Converged after' \
                | sed 's/.*Converged after \([0-9]*\) iterations.*/\1/' || echo "?")
        fi
        iters_lin=$("$eds_tool" -i "$input_eds" -s "$input_seds" -o "$out_lin" -l "$l" 2>&1 >/dev/null \
            | grep 'Converged after' \
            | sed 's/.*Converged after \([0-9]*\) iterations.*/\1/' || echo "?")

        # Cartesian timing (low variability only)
        if [ "$run_cart" -eq 1 ]; then
            local scenario_cart="chain_v${var_tag}_cartesian"
            bench_log "scenario=$scenario_cart  (N=$n_reps, l=$l, iters=$iters_cart)"
            run_bench_scenario "$n_reps" \
                "$eds_tool" -i "$input_eds" -o "$out_cart" -l "$l"
            local tp_cart
            tp_cart=$(compute_throughput "$size_mb" "$BENCH_RUNTIME_S")
            write_csv_row "$csv" "$ts" "$preset" "$scenario_cart" \
                "eds2leds" "$size_mb" "$n_reps" "$tp_cart"
            bench_log "  [cartesian v=$var] median=${BENCH_RUNTIME_S}s  ±${BENCH_RUNTIME_STDDEV_S}s  p99=${BENCH_RUNTIME_P99_S}s  mem=${BENCH_PEAK_MEMORY_MB}MB  throughput=${tp_cart}MB/s  iters=$iters_cart"
        fi

        # Linear timing (all variability values)
        local scenario_lin="chain_v${var_tag}_linear"
        bench_log "scenario=$scenario_lin  (N=$n_reps, l=$l, iters=$iters_lin)"
        run_bench_scenario "$n_reps" \
            "$eds_tool" -i "$input_eds" -s "$input_seds" -o "$out_lin" -l "$l"
        local tp_lin
        tp_lin=$(compute_throughput "$size_mb" "$BENCH_RUNTIME_S")
        write_csv_row "$csv" "$ts" "$preset" "$scenario_lin" \
            "eds2leds" "$size_mb" "$n_reps" "$tp_lin"
        bench_log "  [linear    v=$var] median=${BENCH_RUNTIME_S}s  ±${BENCH_RUNTIME_STDDEV_S}s  p99=${BENCH_RUNTIME_P99_S}s  mem=${BENCH_PEAK_MEMORY_MB}MB  throughput=${tp_lin}MB/s  iters=$iters_lin"
    done
}
