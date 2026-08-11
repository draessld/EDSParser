#!/bin/bash
# Drive the whole M. tuberculosis experiment: build each panel, derive its
# long-allele-filtered twin, run the l sweep on both, and bundle the results.
#
# Usage:
#   run_tb_experiment.sh [base]
#
#   base    dataset root (default $HOME/raid_storage/Data/tb)
#
# Env:
#   PANELS       isolate counts to build      (default "100 500 1141")
#   L_VALUES     context lengths to sweep     (default "10 20 50 100")
#   MAX_ALLELE   allele length filter in bp   (default 50; set 0 to skip the twin)
#   MEM_CAP      per-run memory ceiling       (default 20G; passed to run_subset_dataset.sh)
#   RESULTS_ROOT where bundles are written    (default $HOME/results)
#   JOBS THREADS passed through to fetch_tb_dataset.sh
#   GZ STATS_LEDS EDZ  passed through to collect_results.sh
#
# Everything underneath is resumable, so re-running after an interruption picks up
# where it stopped: assemblies and per-isolate calls are pooled and shared across
# panels, and each stage skips work whose output already exists.
#
# Expect, per panel, roughly: download 4.5 MB/isolate, one minimap2 alignment per
# isolate, then seconds per l value. At 100 isolates the l sweep was 0.6 s and
# 69 MB per l; cost is driven by isolate count, not by l.
set -u

BASE="${1:-$HOME/raid_storage/Data/tb}"
PANELS="${PANELS:-100 500 1141}"
L_VALUES="${L_VALUES:-10 20 50 100}"
MAX_ALLELE="${MAX_ALLELE:-50}"
MEM_CAP="${MEM_CAP:-20G}"
RESULTS_ROOT="${RESULTS_ROOT:-$HOME/results}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FETCH="$SCRIPT_DIR/fetch_tb_dataset.sh"
ALLELE="$SCRIPT_DIR/make_allele_subset.sh"
RUN="$SCRIPT_DIR/run_subset_dataset.sh"
COLLECT="$SCRIPT_DIR/collect_results.sh"
for s in "$FETCH" "$ALLELE" "$RUN" "$COLLECT"; do
    [[ -r "$s" ]] || { echo "missing companion script: $s" >&2; exit 1; }
done

for t in vcf2eds eds2leds edsparser-stats; do
    command -v "$t" >/dev/null || { echo "$t not on PATH — run 'make install' first" >&2; exit 1; }
done
# The complement fix (20d8ff1, 2026-08-04) changes results at <= 63 paths and is
# the whole reason for re-running; refuse to produce another contaminated batch.
#
# This used to probe for the --estimate-memory flag (2026-07-30) as a proxy for
# "new enough". That left a 5-day window: a binary built between Jul 30 and
# Aug 4 has the flag AND the bug, and would sail through. Gate on the commit
# date the binary reports instead. ISO-8601 UTC sorts lexicographically, so a
# string compare is a date compare.
REQUIRED_COMMIT_DATE="2026-08-04T14:16:54Z"
for t in vcf2eds eds2leds edsparser-stats; do
    if ! "$t" --version >/dev/null 2>&1; then
        echo "$t has no --version — it predates the provenance stamp; rebuild and 'make install'" >&2
        exit 1
    fi
    v=$("$t" --version 2>/dev/null)
    d=$(awk -F= '/^COMMIT_DATE=/{print $2}' <<<"$v")
    if [[ -z "$d" || "$d" == "unknown" ]]; then
        echo "$t reports no build commit date — refusing to run" >&2
        exit 1
    fi
    if [[ "$d" < "$REQUIRED_COMMIT_DATE" ]]; then
        echo "$t was built from $(awk -F= '/^COMMIT=/{print $2}' <<<"$v") ($d)," >&2
        echo "  which predates the complement fix ($REQUIRED_COMMIT_DATE)." >&2
        echo "  It silently produces l-EDS containing strings no genome carries." >&2
        echo "  Pull and 'make install' before running." >&2
        exit 1
    fi
    if [[ "$(awk -F= '/^DIRTY=/{print $2}' <<<"$v")" == "1" ]]; then
        echo "warning: $t was built from a dirty tree — results are not reproducible from $(awk -F= '/^COMMIT=/{print $2}' <<<"$v") alone" >&2
    fi
done

started=$(date +%s)
echo "############ TB experiment ############"
echo "  base:     $BASE"
echo "  panels:   $PANELS"
echo "  l values: $L_VALUES"
echo "  filter:   $( ((MAX_ALLELE)) && echo "alleles <= ${MAX_ALLELE} bp (twin dataset)" || echo "none" )"
echo "  results:  $RESULTS_ROOT"
echo

for n in $PANELS; do
    echo "######## panel $n ########"
    mkdir -p "$BASE/logs"
    flog="$BASE/logs/fetch_panel_${n}.log"
    bash "$FETCH" "$BASE" "$n" 2>&1 | tee "$flog"
    if (( ${PIPESTATUS[0]} != 0 )); then   # tee's status would always be 0
        # Without this the reason vanishes: the driver used to report only that
        # fetch failed, leaving the actual download error unseen.
        echo "[panel $n] fetch FAILED — last lines of $flog:" >&2
        tail -15 "$flog" | sed 's/^/    /' >&2
        continue
    fi

    # panel_<n> may be capped below the request when fewer genomes exist; find it.
    panel="$BASE/panel_${n}"
    [[ -d "$panel" ]] || panel=$(ls -d "$BASE"/panel_* 2>/dev/null | tail -1)
    [[ -d "$panel" ]] || { echo "[panel $n] no panel directory produced" >&2; continue; }
    actual=$(basename "$panel"); actual="${actual#panel_}"

    targets=("$panel:tb_p${actual}")
    if (( MAX_ALLELE > 0 )); then
        twin="${panel}_snv${MAX_ALLELE}"
        bash "$ALLELE" "$panel" "$twin" "$MAX_ALLELE" \
            && targets+=("$twin:tb_p${actual}_snv${MAX_ALLELE}")
    fi

    for t in "${targets[@]}"; do
        dir="${t%%:*}"; name="${t##*:}"
        echo "---- $name ----"
        MEM_CAP="$MEM_CAP" bash "$RUN" "$dir" $L_VALUES \
            || echo "[$name] sweep reported failures — continuing" >&2
        bash "$COLLECT" "$dir" "$name" "$RESULTS_ROOT" \
            || echo "[$name] collect FAILED" >&2
    done
done

echo
echo "############ done in $(( ($(date +%s) - started) / 60 )) min ############"
ls -1 "$RESULTS_ROOT"/*_results.tar.gz 2>/dev/null | sed 's/^/  /'
cat <<EOF

Copy back with:
  scp '<server>:$RESULTS_ROOT/*_results.tar.gz' .
  for f in *_results.tar.gz; do tar xzf "\$f" -C <edsparser>/experiments/results/; done
EOF
