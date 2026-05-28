#!/bin/bash
# Benchmark: eds2leds linear transform (source-aware, phasing-preserving merging).

run_scenario_eds2leds_linear() {
    local tmpdir="$1" csv="$2" ts="$3" preset="$4" n_reps="$5"
    shift 5

    local gen_tool eds_tool
    gen_tool=$(find_tool genrandomeds) || { bench_err "genrandomeds not found"; return 1; }
    eds_tool=$(find_tool eds2leds)     || { bench_err "eds2leds not found"; return 1; }

    for ref_size_mb in "$@"; do
        local input_eds="$tmpdir/linear_input_${ref_size_mb}mb.eds"
        local input_seds="$tmpdir/linear_input_${ref_size_mb}mb.seds"
        local output_leds="$tmpdir/linear_output_${ref_size_mb}mb.leds"
        local scenario="eds2leds_linear_${ref_size_mb}mb"

        bench_log "generating input for $scenario ..."
        "$gen_tool" --ref-size-mb "$ref_size_mb" --seed 44 --min-context 5 \
            -o "$input_eds" 2>/dev/null

        bench_log "scenario=$scenario  (N=$n_reps)"
        run_bench_scenario "$n_reps" \
            "$eds_tool" -i "$input_eds" -s "$input_seds" -o "$output_leds" -l 5

        local size_mb throughput
        size_mb=$(file_size_mb "$input_eds")
        throughput=$(compute_throughput "$size_mb" "$BENCH_RUNTIME_S")

        write_csv_row "$csv" "$ts" "$preset" "$scenario" \
            "eds2leds" "$size_mb" "$n_reps" "$throughput"
        bench_log "  median=${BENCH_RUNTIME_S}s  ±${BENCH_RUNTIME_STDDEV_S}s  p99=${BENCH_RUNTIME_P99_S}s  mem=${BENCH_PEAK_MEMORY_MB}MB  throughput=${throughput}MB/s"
    done
}
