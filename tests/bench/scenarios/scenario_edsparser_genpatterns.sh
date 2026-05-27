#!/bin/bash
# Benchmark: edsparser-genpatterns pattern generation throughput.

run_scenario_edsparser_genpatterns() {
    local tmpdir="$1" csv="$2" ts="$3" preset="$4" n_reps="$5"
    shift 5

    local gen_tool pat_tool
    gen_tool=$(find_tool genrandomeds)          || { bench_err "genrandomeds not found"; return 1; }
    pat_tool=$(find_tool edsparser-genpatterns) || { bench_err "edsparser-genpatterns not found"; return 1; }

    for ref_size_mb in "$@"; do
        local input_eds="$tmpdir/genpatterns_input_${ref_size_mb}mb.eds"
        local output_patterns="$tmpdir/genpatterns_output_${ref_size_mb}mb.txt"
        local scenario="edsparser_genpatterns_${ref_size_mb}mb"

        bench_log "generating input for $scenario ..."
        "$gen_tool" --ref-size-mb "$ref_size_mb" --seed 46 --min-context 5 \
            -o "$input_eds" 2>/dev/null

        bench_log "scenario=$scenario  (N=$n_reps)"
        run_bench_scenario "$n_reps" \
            "$pat_tool" -i "$input_eds" -o "$output_patterns" -n 1000 -l 15

        local size_mb throughput
        size_mb=$(file_size_mb "$input_eds")
        throughput=$(compute_throughput "$size_mb" "$BENCH_RUNTIME_S")

        write_csv_row "$csv" "$ts" "$preset" "$scenario" \
            "edsparser-genpatterns" "$size_mb" "$BENCH_RUNTIME_S" "$BENCH_PEAK_MEMORY_MB" "$throughput"
        bench_log "  runtime=${BENCH_RUNTIME_S}s  memory=${BENCH_PEAK_MEMORY_MB}MB  throughput=${throughput}MB/s"
    done
}
