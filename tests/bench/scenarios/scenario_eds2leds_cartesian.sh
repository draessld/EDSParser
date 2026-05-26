#!/bin/bash
# Benchmark: eds2leds cartesian transform (no sources, cross-product merging).

run_scenario_eds2leds_cartesian() {
    local tmpdir="$1" csv="$2" ts="$3" preset="$4" n_reps="$5" ref_size_mb="$6"

    local gen_tool eds_tool
    gen_tool=$(find_tool genrandomeds) || { bench_err "genrandomeds not found"; return 1; }
    eds_tool=$(find_tool eds2leds)     || { bench_err "eds2leds not found"; return 1; }

    local input_eds="$tmpdir/cart_input_${ref_size_mb}mb.eds"
    local output_leds="$tmpdir/cart_output_${ref_size_mb}mb.leds"
    local scenario="eds2leds_cartesian_${ref_size_mb}mb"

    bench_log "generating input for $scenario ..."
    "$gen_tool" --ref-size-mb "$ref_size_mb" --seed 43 --min-context 5 \
        -o "$input_eds" 2>/dev/null

    bench_log "scenario=$scenario  (N=$n_reps)"
    run_bench_scenario "$n_reps" \
        "$eds_tool" -i "$input_eds" -o "$output_leds" -l 5

    local size_mb throughput
    size_mb=$(file_size_mb "$input_eds")
    throughput=$(compute_throughput "$size_mb" "$BENCH_RUNTIME_S")

    write_csv_row "$csv" "$ts" "$preset" "$scenario" \
        "eds2leds" "$size_mb" "$BENCH_RUNTIME_S" "$BENCH_PEAK_MEMORY_MB" "$throughput"
    bench_log "  runtime=${BENCH_RUNTIME_S}s  memory=${BENCH_PEAK_MEMORY_MB}MB  throughput=${throughput}MB/s"
}
