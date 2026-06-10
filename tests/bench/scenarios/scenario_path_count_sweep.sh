#!/bin/bash
# Benchmark: eds2leds linear merge at increasing path counts.
# Fixed: ref_size, variability=0.05, l=5, --min-context 5.
# Varies: --min-alternatives / --max-alternatives (controls number of paths).
# Cartesian is unaffected by path count so only linear is tested here.

run_scenario_path_count_sweep() {
    local tmpdir="$1" csv="$2" ts="$3" preset="$4" n_reps="$5" ref_size_mb="$6"
    shift 6
    # Each element is "min_alt:max_alt" — genrandomeds sets n_paths = max(max_alt, 3)
    local configs=("$@")

    local gen_tool eds_tool
    gen_tool=$(find_tool genrandomeds) || { bench_err "genrandomeds not found"; return 1; }
    eds_tool=$(find_tool eds2leds)     || { bench_err "eds2leds not found"; return 1; }

    local seed=300
    for cfg in "${configs[@]}"; do
        local min_alt max_alt
        min_alt="${cfg%%:*}"
        max_alt="${cfg##*:}"

        local input_eds="$tmpdir/paths_${min_alt}_${max_alt}_${ref_size_mb}mb.eds"
        local input_seds="$tmpdir/paths_${min_alt}_${max_alt}_${ref_size_mb}mb.seds"
        local out_lin="$tmpdir/paths_${min_alt}_${max_alt}_lin.leds"
        local scenario="path_count_min${min_alt}_max${max_alt}_linear"

        bench_log "generating input for $scenario (min-alt=$min_alt, max-alt=$max_alt) ..."
        "$gen_tool" --ref-size-mb "$ref_size_mb" --seed $((seed++)) \
            --variability 0.05 --min-context 5 \
            --min-alternatives "$min_alt" --max-alternatives "$max_alt" \
            -o "$input_eds" 2>/dev/null

        local size_mb
        size_mb=$(file_size_mb "$input_eds")

        bench_log "scenario=$scenario  (N=$n_reps)"
        run_bench_scenario "$n_reps" \
            "$eds_tool" -i "$input_eds" -s "$input_seds" -o "$out_lin" -l 5
        local throughput
        throughput=$(compute_throughput "$size_mb" "$BENCH_RUNTIME_S")
        write_csv_row "$csv" "$ts" "$preset" "$scenario" \
            "eds2leds" "$size_mb" "$n_reps" "$throughput"
        bench_log "  median=${BENCH_RUNTIME_S}s  ±${BENCH_RUNTIME_STDDEV_S}s  p99=${BENCH_RUNTIME_P99_S}s  mem=${BENCH_PEAK_MEMORY_MB}MB  throughput=${throughput}MB/s"
    done
}
