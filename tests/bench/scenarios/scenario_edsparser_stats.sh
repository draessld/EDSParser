#!/bin/bash
# Benchmark: edsparser-stats statistics computation speed.

run_scenario_edsparser_stats() {
    local tmpdir="$1" csv="$2" ts="$3" preset="$4" n_reps="$5"
    shift 5
    local sizes_mb=("$@")

    local gen_tool stats_tool
    gen_tool=$(find_tool genrandomeds)     || { bench_err "genrandomeds not found"; return 1; }
    stats_tool=$(find_tool edsparser-stats) || { bench_err "edsparser-stats not found"; return 1; }

    local seed=45
    for ref_size in "${sizes_mb[@]}"; do
        local input_eds="$tmpdir/stats_input_${ref_size}mb.eds"
        local scenario="edsparser_stats_${ref_size}mb"

        bench_log "generating input for $scenario ..."
        "$gen_tool" --ref-size-mb "$ref_size" --seed $((seed++)) --min-context 5 \
            -o "$input_eds" 2>/dev/null

        bench_log "scenario=$scenario  (N=$n_reps)"
        run_bench_scenario "$n_reps" \
            "$stats_tool" -i "$input_eds"

        local size_mb throughput
        size_mb=$(file_size_mb "$input_eds")
        throughput=$(compute_throughput "$size_mb" "$BENCH_RUNTIME_S")

        write_csv_row "$csv" "$ts" "$preset" "$scenario" \
            "edsparser-stats" "$size_mb" "$BENCH_RUNTIME_S" "$BENCH_PEAK_MEMORY_MB" "$throughput"
        bench_log "  runtime=${BENCH_RUNTIME_S}s  memory=${BENCH_PEAK_MEMORY_MB}MB  throughput=${throughput}MB/s"
    done
}
