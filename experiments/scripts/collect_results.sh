#!/bin/bash
# Collect everything worth keeping from a dataset directory on the server into a
# self-contained results folder (+ tarball) to copy back into experiments/results/.
#
# The dataset itself (VCF, EDS, l-EDS) stays on the server — only logs, stats and
# size measurements travel, which is a few MB instead of many GB.
#
# Usage:
#   collect_results.sh <data_base> [dataset_name] [out_root]
#
#   data_base      dataset root as built by run_subset_dataset.sh / fetch_tb_dataset.sh,
#                  i.e. containing vcf/ ref/ eds/ leds_l<N>/
#   dataset_name   name of the results folder (default: basename of data_base)
#   out_root       where to write it (default: $HOME/results)
#
# Env:
#   STATS_LEDS=0   skip edsparser-stats over the .leds files (they can be large;
#                  on by default because eds2leds/stats/leds_l<N>.csv feeds the notebooks)
#   GZ=0           skip gzip size measurements (slow on multi-GB .leds)
#   EDZ=0          skip the EDZ re-encode column of sources_compression.csv
#   TAR=0          leave the folder uncompressed
#
# Layout produced (mirrors experiments/results/yeast1011_50):
#   <name>/vcf2eds/*.vcf2eds.log          per-chromosome VCF→EDS logs
#   <name>/vcf2eds/stats/                 edsparser-stats over the EDS
#   <name>/eds2leds/l<N>/*.out            per-chromosome EDS→l-EDS logs (linear)
#   <name>/eds2leds/l<N>_cart/*.out       the same, cartesian merge
#   <name>/eds2leds/stats/leds_l<N>.csv   edsparser-stats over the l-EDS
#   <name>/other/leds_runs.tsv            per-run outcome, incl. runs killed at the cap
#   <name>/other/                         disk/gzip/EDZ size measurements
#   <name>/MANIFEST.txt                   what was collected, from where, with what binary
#
# The `_cart` directories are the cartesian arm written by run_subset_dataset.sh; they
# flow through unchanged because every loop here keys off the leds_l* glob, and their
# stats land in leds_l<N>_cart.csv alongside the linear leds_l<N>.csv. Feed the pair to
# compare_merge_modes.py.
set -u

# Resolve before any cd — BASH_SOURCE is relative when invoked by a relative path.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BASE="${1:?usage: collect_results.sh <data_base> [dataset_name] [out_root]}"
NAME="${2:-$(basename "$BASE")}"
OUT_ROOT="${3:-$HOME/results}"
STATS_LEDS="${STATS_LEDS:-1}"
GZ="${GZ:-1}"
EDZ="${EDZ:-1}"
TAR="${TAR:-1}"

[[ -d "$BASE" ]] || { echo "no such dataset dir: $BASE" >&2; exit 1; }
OUT="$OUT_ROOT/$NAME"
mkdir -p "$OUT"/{vcf2eds/stats,eds2leds/stats,other} || exit 1

STATS_TOOL="$(command -v edsparser-stats || echo "$HOME/.local/bin/edsparser-stats")"
XFORM_TOOL="$(command -v edsparser-source-transform || echo "$HOME/.local/bin/edsparser-source-transform")"
[[ -x "$STATS_TOOL" ]] || { echo "edsparser-stats not found — stats will be skipped" >&2; STATS_TOOL=""; }

cd "$BASE" || exit 1
shopt -s nullglob

echo "=== collecting $BASE → $OUT ==="

# ── transformation logs ──────────────────────────────────────────────────────
logs=(eds/*.vcf2eds.log)
(( ${#logs[@]} )) && cp "${logs[@]}" "$OUT/vcf2eds/"
echo "    vcf2eds logs: ${#logs[@]}"

# leds_l<N> is the linear arm, leds_l<N>_cart the cartesian one; the suffix is carried
# through into the collected directory and stats names rather than stripped, so the two
# arms stay distinguishable downstream.
l_dirs=(leds_l*)
for d in "${l_dirs[@]}"; do
    [[ -d "$d" ]] || continue
    l="${d#leds_l}"
    mkdir -p "$OUT/eds2leds/l$l"
    outs=("$d"/*.out)
    (( ${#outs[@]} )) && cp "${outs[@]}" "$OUT/eds2leds/l$l/"
    echo "    eds2leds l=$l logs: ${#outs[@]}"
done

# Runs killed at the memory cap leave no output file, so without this the cartesian
# arm's most informative cells would just be missing rows.
[[ -s leds_runs.tsv ]] && cp leds_runs.tsv "$OUT/other/" && \
    echo "    leds_runs.tsv: $(( $(wc -l < leds_runs.tsv) - 1 )) runs"

# ── stats over the EDS ───────────────────────────────────────────────────────
# run_stats.sh already writes these into eds/stats when it has been run; otherwise
# produce them here so the results folder is complete on its own.
if [[ -n "$STATS_TOOL" ]]; then
    mkdir -p eds/stats
    combined="$OUT/vcf2eds/stats/all_stats.csv"
    : > "$combined"
    wrote_header=0
    for eds in eds/*.eds; do
        c=$(basename "$eds" .eds)
        csv="eds/stats/${c}.stats.csv"
        txt="eds/stats/${c}.stats.txt"
        if [[ ! -s "$csv" ]]; then
            args=(-i "$eds"); [[ -s "eds/${c}.seds" ]] && args+=(-s "eds/${c}.seds")
            "$STATS_TOOL" "${args[@]}" --verbose > "$txt" 2>&1
            "$STATS_TOOL" "${args[@]}" --csv > "$csv" 2>/dev/null \
                || { echo "      [$c] --csv unsupported; keeping text only" >&2; rm -f "$csv"; }
        fi
        [[ -s "$csv" ]] || continue
        if (( wrote_header == 0 )); then head -n1 "$csv" > "$combined"; wrote_header=1; fi
        tail -n +2 "$csv" >> "$combined"
    done
    s=(eds/stats/*); (( ${#s[@]} )) && cp "${s[@]}" "$OUT/vcf2eds/stats/" 2>/dev/null
    echo "    EDS stats: $(( $(wc -l < "$combined") - 1 )) rows"
fi

# ── stats over the l-EDS ─────────────────────────────────────────────────────
# One CSV per l, with a leading chrom column — the shape the notebooks expect.
if [[ -n "$STATS_TOOL" && "$STATS_LEDS" == "1" ]]; then
    for d in "${l_dirs[@]}"; do
        [[ -d "$d" ]] || continue
        l="${d#leds_l}"
        csv="$OUT/eds2leds/stats/leds_l${l}.csv"
        : > "$csv"
        wrote_header=0
        for leds in "$d"/*.leds; do
            c=$(basename "$leds" .leds)
            args=(-i "$leds"); [[ -s "$d/${c}.seds" ]] && args+=(-s "$d/${c}.seds")
            row=$("$STATS_TOOL" "${args[@]}" --csv 2>/dev/null) || continue
            [[ -z "$row" ]] && continue
            if (( wrote_header == 0 )); then
                echo "chrom,$(head -n1 <<<"$row")" > "$csv"; wrote_header=1
            fi
            tail -n +2 <<<"$row" | sed "s|^|${c},|" >> "$csv"
        done
        (( wrote_header )) && echo "    l-EDS stats l=$l: $(( $(wc -l < "$csv") - 1 )) rows" \
                           || rm -f "$csv"
    done
fi

# ── size measurements ────────────────────────────────────────────────────────
dirs=(vcf ref eds "${l_dirs[@]}")
present=(); for d in "${dirs[@]}"; do [[ -d "$d" ]] && present+=("$d"); done

du -sh "${present[@]}" > "$OUT/other/totals.txt" 2>/dev/null
du -ab "${present[@]}" > "$OUT/other/all_sizes_bytes.txt" 2>/dev/null
for d in "${present[@]}"; do
    du -ah "$d" > "$OUT/other/${d}_disk_size.txt" 2>/dev/null
done
echo "    size measurements written"

if [[ "$GZ" == "1" ]]; then
    echo "    gzip sizes (set GZ=0 to skip — slow on large .leds)"
    for d in "${present[@]}"; do
        out="$OUT/other/${d}_gz_bytes.txt"; : > "$out"
        for f in "$d"/*; do
            [[ -f "$f" ]] || continue
            printf '%s\t%s\n' "$(gzip -6 -c "$f" | wc -c)" "$f" >> "$out"
        done
    done
fi

# sources: raw vs gzip vs EDZ, the comparison the sources-format section uses
sc="$OUT/other/sources_compression.csv"
echo "file,seds_bytes,gz_bytes,edz_bytes" > "$sc"
seds_files=(eds/*.seds)
for d in "${l_dirs[@]}"; do
    [[ -d "$d" ]] && seds_files+=("$d"/*.seds)
done
for seds in "${seds_files[@]}"; do
    [[ -f "$seds" ]] || continue
    raw=$(stat -c%s "$seds")
    gzb=""; [[ "$GZ" == "1" ]] && gzb=$(gzip -6 -c "$seds" | wc -c)
    edzb=""
    if [[ "$EDZ" == "1" && -x "$XFORM_TOOL" ]]; then
        tmp=$(mktemp -u --suffix=.edz)
        "$XFORM_TOOL" -i "$seds" -o "$tmp" --to edz --sparse >/dev/null 2>&1 \
            && edzb=$(stat -c%s "$tmp" 2>/dev/null)
        rm -f "$tmp"
    fi
    echo "${seds},${raw},${gzb},${edzb}" >> "$sc"
done
echo "    sources_compression.csv: $(( $(wc -l < "$sc") - 1 )) source files"

# ── manifest ─────────────────────────────────────────────────────────────────
l_values=(); modes=()
for d in "${l_dirs[@]}"; do
    [[ -d "$d" ]] || continue
    l="${d#leds_l}"
    if [[ "$l" == *_cart ]]; then l="${l%_cart}"; m=cartesian; else m=linear; fi
    [[ " ${l_values[*]-} " == *" $l "* ]] || l_values+=("$l")
    [[ " ${modes[*]-} "    == *" $m "* ]] || modes+=("$m")
done
{
    echo "dataset:     $NAME"
    echo "source:      $(hostname):$BASE"
    echo "collected:   $(date '+%Y-%m-%d %H:%M:%S %Z')"
    echo "eds2leds:    $(command -v eds2leds || echo 'not on PATH')"
    echo "binary mtime: $(stat -c%y "$(command -v eds2leds)" 2>/dev/null | cut -d. -f1)"
    echo "repo commit: $(git -C "${EDSPARSER_ROOT:-$SCRIPT_DIR}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo
    echo "l values:    ${l_values[*]:-none}"
    echo "merge modes: ${modes[*]:-none}"
    echo "chromosomes: $(ls eds/*.eds 2>/dev/null | wc -l)"
    # num_paths decides whether the merge takes the bitset fast path (<= 63), so
    # it is the first thing to check when comparing runs across the fix.
    echo "paths:       $(awk -F, 'NR==1{for(i=1;i<=NF;i++) if($i=="num_paths") c=i}
                                 NR==2 && c{print $c; exit}' \
                          "$OUT/vcf2eds/stats/all_stats.csv" 2>/dev/null || echo unknown)"
    echo
    echo "NOTE: results produced at 63 paths or fewer by a binary older than 20d8ff1"
    echo "measure the complement-source bug and must not be used."
} > "$OUT/MANIFEST.txt"

# ── package ──────────────────────────────────────────────────────────────────
if [[ "$TAR" == "1" ]]; then
    tar czf "$OUT_ROOT/${NAME}_results.tar.gz" -C "$OUT_ROOT" "$NAME" \
        && echo "=== $OUT_ROOT/${NAME}_results.tar.gz ($(du -h "$OUT_ROOT/${NAME}_results.tar.gz" | cut -f1)) ==="
    echo
    echo "Copy it back with:"
    echo "  scp <server>:$OUT_ROOT/${NAME}_results.tar.gz ."
    echo "  tar xzf ${NAME}_results.tar.gz -C <edsparser>/experiments/results/"
else
    echo "=== $OUT ==="
fi
