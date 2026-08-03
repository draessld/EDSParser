#!/bin/bash
# Runs edsparser-stats over every EDS in the 1000HGp3 eds directory.
# Sequential (stats is fast and read-only — no per-chromosome screens needed).
# Writes a per-chromosome verbose text log plus one combined CSV.
#
# Options:
#   --force   Recompute stats even if a stats file already exists.
#
# Env:
#   EDS_DIR   Override input dir (default $HOME/raid_storage/Data/1000HGp3/eds).

force=0
for arg in "$@"; do
    [[ "$arg" == "--force" ]] && force=1
done

OUT_BASE="${OUT_BASE:-$HOME/raid_storage/Data/1000HGp3}"
EDS_DIR="${EDS_DIR:-$OUT_BASE/eds}"
OUT_DIR="$EDS_DIR/stats"
COMBINED_CSV="$OUT_DIR/all_stats.csv"

STATS_TOOL="$(command -v edsparser-stats || echo "$HOME/.local/bin/edsparser-stats")"
if [[ ! -x "$STATS_TOOL" ]]; then
    echo "edsparser-stats not found on PATH or in ~/.local/bin" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
: > "$COMBINED_CSV"   # truncate; header written from the first chromosome

echo "=== EDS stats batch run — $(date '+%Y-%m-%d %H:%M:%S') ==="
echo "    input:    $EDS_DIR"
echo "    output:   $OUT_DIR"

processed=0
wrote_header=0
for eds_file in "$EDS_DIR"/*.eds; do
    [[ -f "$eds_file" ]] || { echo "No EDS files found in $EDS_DIR — run run_eds.sh first"; exit 1; }
    chrom=$(basename "$eds_file" .eds)
    seds_file="$EDS_DIR/${chrom}.seds"
    txt_out="$OUT_DIR/${chrom}.stats.txt"
    csv_out="$OUT_DIR/${chrom}.stats.csv"

    if [[ -f "$txt_out" && $force -eq 0 ]]; then
        echo "[chr${chrom}] SKIP — stats already exist: $txt_out"
        # Still fold the existing CSV (minus header) into the combined file.
        if [[ -f "$csv_out" ]]; then
            [[ $wrote_header -eq 0 ]] && { head -n1 "$csv_out" > "$COMBINED_CSV"; wrote_header=1; }
            tail -n +2 "$csv_out" >> "$COMBINED_CSV"
        fi
        continue
    fi

    # Pass sources if a matching .seds exists (enables path-count stats).
    seds_args=()
    [[ -f "$seds_file" ]] && seds_args=(-s "$seds_file")

    echo "[chr${chrom}] START — $(date '+%H:%M:%S')"
    "$STATS_TOOL" -i "$eds_file" "${seds_args[@]}" --verbose > "$txt_out" 2>&1
    ec_txt=$?
    "$STATS_TOOL" -i "$eds_file" "${seds_args[@]}" --csv > "$csv_out" 2>/dev/null
    ec_csv=$?

    if [[ $ec_txt -ne 0 || $ec_csv -ne 0 ]]; then
        echo "[chr${chrom}] FAIL — see $txt_out" >&2
        continue
    fi

    # Combined CSV: header once, then each chromosome's data row.
    if [[ $wrote_header -eq 0 ]]; then
        head -n1 "$csv_out" > "$COMBINED_CSV"
        wrote_header=1
    fi
    tail -n +2 "$csv_out" >> "$COMBINED_CSV"

    echo "[chr${chrom}] DONE — $(du -sh "$eds_file" | cut -f1) EDS"
    processed=$((processed + 1))
done

echo "=== STATS DONE — $(date '+%Y-%m-%d %H:%M:%S') — processed ${processed} file(s) ==="
echo "Per-chromosome logs: $OUT_DIR/*.stats.txt"
echo "Combined CSV:        $COMBINED_CSV"
