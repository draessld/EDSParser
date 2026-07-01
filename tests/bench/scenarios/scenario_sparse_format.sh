#!/bin/bash
# Benchmark: sparse source formats (SEDS_SPARSE / EDZ_SPARSE) vs dense.
#
# Measures for each input size:
#
#   1. vcf2eds write speed — all four formats for the same EDS content.
#      Uses _generate_synthetic_vcf (shared with scenario_edz_format.sh).
#
#   2. Source file sizes — bytes written by each format; sparse vs dense ratio.
#
#   3. eds2leds linear read speed — eds2leds with each source format as input.
#      Sparse formats skip reference entries in the file, so the LRU cache
#      sees fewer unique entries.  Expected: sparse ≈ dense read speed
#      (rank-lookup is O(1)) with smaller I/O footprint.
#
# Fixed: n_samples=10, variability=0.05, l=5.
# The variability=0.05 produces ~5% degenerate positions (non-universal entries)
# so sparse formats store ~20× fewer entries than dense → clear size savings.

run_scenario_sparse_format() {
    local tmpdir="$1" csv="$2" ts="$3" preset="$4" n_reps="$5"
    shift 5
    local sizes_mb=("$@")

    local vcf_tool eds_tool
    vcf_tool=$(find_tool vcf2eds)    || { bench_err "vcf2eds not found";   return 1; }
    eds_tool=$(find_tool eds2leds)   || { bench_err "eds2leds not found";  return 1; }

    local var=0.05
    local l=5
    local n_samples=10
    local seed=1300

    for ref_size_mb in "${sizes_mb[@]}"; do
        local n_bases=$(( ref_size_mb * 1024 * 1024 ))
        local n_variants
        n_variants=$(awk "BEGIN { printf \"%d\", $n_bases * $var }")

        local base="$tmpdir/sparse_${ref_size_mb}mb"
        local fasta_file="${base}.fa"
        local vcf_file="${base}.vcf"

        bench_log "=== sparse_format: ref=${ref_size_mb}MB  samples=${n_samples}  var=${var} ==="
        bench_log "  generating synthetic VCF (${ref_size_mb}MB genome, ${n_samples} samples, ${n_variants} variants) ..."
        _generate_synthetic_vcf "$fasta_file" "$vcf_file" "$n_bases" "$n_samples" "$n_variants"

        local fasta_size_mb
        fasta_size_mb=$(file_size_mb "$fasta_file")

        # ── 1. vcf2eds write benchmark — all four formats ─────────────────────
        declare -A fmt_eds fmt_src

        local fmts=("seds" "seds-sparse" "edz" "edz-sparse")
        local exts=(".seds" ".seds" ".edz" ".edz")

        for i in "${!fmts[@]}"; do
            local fmt="${fmts[$i]}"
            local ext="${exts[$i]}"
            local sc="sparse_format_vcf_write_${fmt//[-]/_}_${ref_size_mb}mb"
            fmt_eds[$fmt]="${base}_${fmt//[-]/_}.eds"
            fmt_src[$fmt]="${base}_${fmt//[-]/_}${ext}"

            bench_log "  scenario=$sc  (N=$n_reps)"
            run_bench_scenario "$n_reps" \
                "$vcf_tool" -i "$vcf_file" -r "$fasta_file" \
                    -o "${fmt_eds[$fmt]}" -s "${fmt_src[$fmt]}" \
                    --source-format "$fmt" 2>/dev/null

            local tp
            tp=$(compute_throughput "$fasta_size_mb" "$BENCH_RUNTIME_S")
            write_csv_row "$csv" "$ts" "$preset" "$sc" \
                "vcf2eds($fmt)" "$fasta_size_mb" "$n_reps" "$tp"
            bench_log "  [$fmt] median=${BENCH_RUNTIME_S}s  ±${BENCH_RUNTIME_STDDEV_S}s  mem=${BENCH_PEAK_MEMORY_MB}MB  tp=${tp}MB/s"
        done

        # ── 2. File size comparison ───────────────────────────────────────────
        bench_log "  [sizes] source files:"
        for fmt in "${fmts[@]}"; do
            local bytes
            bytes=$(stat -c "%s" "${fmt_src[$fmt]}" 2>/dev/null || echo 0)
            local size_mb
            size_mb=$(awk "BEGIN { printf \"%.4f\", $bytes/1048576 }")
            bench_log "    $fmt: ${size_mb}MB"
        done

        # Sparse-vs-dense size ratios
        local seds_bytes seds_sp_bytes edz_bytes edz_sp_bytes
        seds_bytes=$(   stat -c "%s" "${fmt_src[seds]}"        2>/dev/null || echo 1)
        seds_sp_bytes=$(stat -c "%s" "${fmt_src[seds-sparse]}" 2>/dev/null || echo 1)
        edz_bytes=$(    stat -c "%s" "${fmt_src[edz]}"         2>/dev/null || echo 1)
        edz_sp_bytes=$( stat -c "%s" "${fmt_src[edz-sparse]}"  2>/dev/null || echo 1)

        local ratio_seds ratio_edz
        ratio_seds=$(awk "BEGIN { printf \"%.2f\", $seds_bytes / $seds_sp_bytes }")
        ratio_edz=$( awk "BEGIN { printf \"%.2f\", $edz_bytes  / $edz_sp_bytes  }")
        bench_log "  [ratio] seds/seds-sparse=${ratio_seds}×  edz/edz-sparse=${ratio_edz}×"

        # ── 3. eds2leds read benchmark — all four formats as source input ─────
        for fmt in "${fmts[@]}"; do
            local sc="sparse_format_leds_${fmt//[-]/_}_${ref_size_mb}mb"
            local out_leds="${base}_leds_${fmt//[-]/_}.leds"

            bench_log "  scenario=$sc  (N=$n_reps)"
            run_bench_scenario "$n_reps" \
                "$eds_tool" -i "${fmt_eds[$fmt]}" -s "${fmt_src[$fmt]}" \
                    -l "$l" -o "$out_leds" 2>/dev/null

            local eds_size_mb
            eds_size_mb=$(file_size_mb "${fmt_eds[$fmt]}")
            local tp
            tp=$(compute_throughput "$eds_size_mb" "$BENCH_RUNTIME_S")
            write_csv_row "$csv" "$ts" "$preset" "$sc" \
                "eds2leds(src=$fmt)" "$eds_size_mb" "$n_reps" "$tp"
            bench_log "  [$fmt read] median=${BENCH_RUNTIME_S}s  ±${BENCH_RUNTIME_STDDEV_S}s  mem=${BENCH_PEAK_MEMORY_MB}MB  tp=${tp}MB/s"
        done

        echo ""
    done
}
