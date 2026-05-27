#!/bin/bash
# Benchmark: genrandomeds generation speed at multiple EDS sizes.

run_scenario_genrandomeds() {
    local tmpdir="$1" csv="$2" ts="$3" preset="$4" n_reps="$5"
    shift 5
    local sizes_mb=("$@")

    local tool
    tool=$(find_tool genrandomeds) || { bench_err "genrandomeds not found"; return 1; }

    for ref_size in "${sizes_mb[@]}"; do
        local out_eds="$tmpdir/genrandomeds_${ref_size}mb.eds"
        local scenario="genrandomeds_${ref_size}mb"

        bench_log "scenario=$scenario  (N=$n_reps)"
        run_bench_scenario "$n_reps" \
            "$tool" --ref-size-mb "$ref_size" --seed 42 --min-context 5 -o "$out_eds"

        local size_mb throughput
        size_mb=$(file_size_mb "$out_eds")
        throughput=$(compute_throughput "$size_mb" "$BENCH_RUNTIME_S")

        write_csv_row "$csv" "$ts" "$preset" "$scenario" \
            "genrandomeds" "$size_mb" "$BENCH_RUNTIME_S" "$BENCH_PEAK_MEMORY_MB" "$throughput"
        bench_log "  runtime=${BENCH_RUNTIME_S}s  memory=${BENCH_PEAK_MEMORY_MB}MB  throughput=${throughput}MB/s"
    done
}
