#!/bin/bash
# Stage 2 master: spawns one screen per chromosome to run EDS → l-EDS (all l-values).
# Run this after run_eds.sh has finished.
# Run inside a screen session, e.g.:  screen -S run_leds
#
# Resumable: on startup it adopts any worker screens (eds2leds_<chrom>) already
# running from a previous, stopped master instead of double-spawning them, and
# skips chromosomes previously marked too-intensive (see MEM_KILL_GB). Per-l-value
# outputs that already exist are skipped by run_chrom_leds.sh, so a plain re-run
# also resumes partially-finished chromosomes.
#
# Env:
#   L_VALUES     space-separated list of context lengths (default "5 10 20 30 40 50 100").
#                Example: L_VALUES="3 5 8" ./run_leds.sh
#   MAX_PARALLEL max chromosome workers running at once (default 4).
#   MIN_FREE_GB  do not launch a new chromosome unless at least this many GiB of
#                RAM are available (default 80). Second safety net below the count cap.
#   MEM_KILL_GB  hard ceiling: if total *used* RAM reaches this, kill the heaviest
#                running eds2leds worker and mark its chromosome "too intensive"
#                (default 450, i.e. clear of the 504 GiB box). Retry those later
#                as a single process (see $TOO_INTENSIVE_LOG).
#   FORCE        1 = recompute outputs that already exist (default: skip complete ones).
#                Incomplete (zero-byte) leftovers from a killed run are always redone.
#   ADMIT_BUDGET_GB  memory-aware admission control (default 380). Before launching a
#                chromosome, its predicted peak (from `eds2leds --estimate-memory`) is
#                added to the predicted peaks of the workers already running; the launch
#                waits until the total fits this budget. This is what stops two heavy
#                chromosomes from starting together — MAX_PARALLEL counts processes,
#                which says nothing about their size (measured spread at l=5: 1.1 GB for
#                chr8 vs 29.3 GB for chr7). Set 0 to disable and fall back to counting.
#   MAX_MEMORY   forwarded to the worker as `eds2leds --max-memory`: a per-run pre-flight
#                that refuses (exit 3, no work done) if one run alone would exceed it.
#                Empty (default) = no pre-flight. Complements ADMIT_BUDGET_GB: this one
#                catches a single impossible run, that one catches bad combinations.
#   BLOCK_SIZE_LEDS  forwarded as `eds2leds --block-size` (e.g. 200M). Bounds each run's
#                peak RAM to roughly one block rather than the whole file (~2.7x slower,
#                byte-identical output). With this set, estimates drop accordingly and
#                many more chromosomes fit at once.
#
# Chromosomes are launched heaviest-first (largest predicted peak), which packs a fixed
# budget better than the lexicographic glob order and gets the risky ones running while
# the machine is empty.
#
# LIVE CONTROLS — while this is running you can retune it without restarting; values are
# re-read before every launch decision (within ~30s):
#     echo 2   > $OUT_BASE/control/max_parallel
#     echo 200 > $OUT_BASE/control/min_free_gb
#     echo 250 > $OUT_BASE/control/admit_budget_gb

# Export so the value reaches each worker spawned via `screen`.
export L_VALUES="${L_VALUES:-5 10 20 30 40 50 100}"

# Concurrency is bounded two ways because a single eds2leds run has been measured
# at up to ~50 GB RSS (large chromosome, higher l): spawning all ~24 at once
# exhausted a 504 GB box (no swap) and triggered the OOM killer.
#   1. MAX_PARALLEL caps how many chromosome workers are alive at once.
#   2. MIN_FREE_GB refuses to launch another chromosome while free RAM is low,
#      so we stay clear of OOM even if per-run peaks are heavier than expected
#      or MAX_PARALLEL is raised.
MAX_PARALLEL="${MAX_PARALLEL:-4}"
MIN_FREE_GB="${MIN_FREE_GB:-150}"
MEM_KILL_GB="${MEM_KILL_GB:-450}"
ADMIT_BUDGET_GB="${ADMIT_BUDGET_GB:-380}"
# Optional finer-grained budget (wins over ADMIT_BUDGET_GB when set). Accounting is
# done in MiB throughout: whole GiB would round every sub-GiB run down to zero and
# quietly disable admission control.
ADMIT_BUDGET_MB="${ADMIT_BUDGET_MB-}"

# Forwarded to each worker (see run_chrom_leds.sh).
export MAX_MEMORY="${MAX_MEMORY-}"
# FORCE=1 recomputes outputs that already exist; by default they are skipped.
export FORCE="${FORCE-}"
export BLOCK_SIZE_LEDS="${BLOCK_SIZE_LEDS-}"

OUT_BASE="${OUT_BASE:-$HOME/raid_storage/Data/1000HGp3}"
export OUT_BASE
EDS_DIR="$OUT_BASE/eds"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Overridable so a sibling master (e.g. run_leds_incremental.sh) can reuse this
# orchestration with a different per-chromosome worker and its own screen names.
MASTER_SCREEN="${MASTER_SCREEN:-run_leds}"
SCREEN_PREFIX="${SCREEN_PREFIX:-eds2leds}"
WORKER_SCRIPT="${WORKER_SCRIPT:-$SCRIPT_DIR/run_chrom_leds.sh}"

# Chromosomes killed by the memory cap are recorded here (marker file per chrom is
# read by run_chrom_leds.sh to stop its remaining l-values; the log is the list to
# retry later as a single process).
TOO_INTENSIVE_DIR="$OUT_BASE/too_intensive"
TOO_INTENSIVE_LOG="$TOO_INTENSIVE_DIR/too_intensive.log"
export TOO_INTENSIVE_DIR

# Live-tunable limits: written once at startup, re-read before each launch decision so
# the run can be throttled (or opened up) from another shell while it works.
CONTROL_DIR="$OUT_BASE/control"
mkdir -p "$CONTROL_DIR"
[[ -f "$CONTROL_DIR/max_parallel" ]]     || echo "$MAX_PARALLEL"     > "$CONTROL_DIR/max_parallel"
[[ -f "$CONTROL_DIR/min_free_gb" ]]      || echo "$MIN_FREE_GB"      > "$CONTROL_DIR/min_free_gb"
[[ -f "$CONTROL_DIR/admit_budget_gb" ]]  || echo "$ADMIT_BUDGET_GB"  > "$CONTROL_DIR/admit_budget_gb"
[[ -z "$ADMIT_BUDGET_MB" || -f "$CONTROL_DIR/admit_budget_mb" ]] || echo "$ADMIT_BUDGET_MB" > "$CONTROL_DIR/admit_budget_mb"

# Re-read the control files; ignore anything that is not a positive integer.
refresh_limits() {
    local v
    v=$(cat "$CONTROL_DIR/max_parallel" 2>/dev/null);    [[ "$v" =~ ^[0-9]+$ ]] && (( v > 0 )) && MAX_PARALLEL="$v"
    v=$(cat "$CONTROL_DIR/min_free_gb" 2>/dev/null);     [[ "$v" =~ ^[0-9]+$ ]] && MIN_FREE_GB="$v"
    v=$(cat "$CONTROL_DIR/admit_budget_gb" 2>/dev/null); [[ "$v" =~ ^[0-9]+$ ]] && ADMIT_BUDGET_GB="$v"
    v=$(cat "$CONTROL_DIR/admit_budget_mb" 2>/dev/null); [[ "$v" =~ ^[0-9]+$ ]] && ADMIT_BUDGET_MB="$v"
}

# The admission budget in MiB (ADMIT_BUDGET_MB wins when set; 0 = disabled).
budget_mib() {
    if [[ -n "$ADMIT_BUDGET_MB" ]]; then echo "$ADMIT_BUDGET_MB"
    else echo $(( ADMIT_BUDGET_GB * 1024 )); fi
}

echo "=== EDS → l-EDS batch run — $(date '+%Y-%m-%d %H:%M:%S') ==="
echo "    l-values: ${L_VALUES}"
echo "    max parallel chromosomes: ${MAX_PARALLEL} | min free RAM to launch: ${MIN_FREE_GB} GiB | hard kill at used >= ${MEM_KILL_GB} GiB"
if (( $(budget_mib) > 0 )); then
    echo "    admission budget: $(budget_mib) MiB of predicted peaks in flight (eds2leds --estimate-memory)"
else
    echo "    admission budget: disabled (counting only)"
fi
[[ -n "$MAX_MEMORY" ]]      && echo "    per-run pre-flight: --max-memory ${MAX_MEMORY} (exit 3 => too-intensive)"
[[ -n "$BLOCK_SIZE_LEDS" ]] && echo "    per-run block mode: --block-size ${BLOCK_SIZE_LEDS} (bounded RAM, ~2.7x slower)"
echo "    live controls: ${CONTROL_DIR}/{max_parallel,min_free_gb,admit_budget_gb}"

# Number of worker screens currently alive. Each screen name is the prefix plus
# "_<chrom>"; the trailing [^[:space:]]+ pins it to a real session token so the
# count is exact (screen -ls lists "<pid>.<name>\t(<state>)").
running_workers() { screen -ls 2>/dev/null | grep -cE "\.${SCREEN_PREFIX}_[^[:space:]]+"; }
# Is a specific worker screen alive? Anchored on trailing whitespace so
# "eds2leds_1" does not match "eds2leds_11". Used to adopt pre-existing workers
# (e.g. survivors of an earlier master run that was stopped) instead of
# double-spawning them.
worker_alive() { screen -ls 2>/dev/null | grep -qE "\.${SCREEN_PREFIX}_$1[[:space:]]"; }
# Available RAM in GiB (the 'available' column also counts reclaimable cache).
free_gb() { free -g | awk '/^Mem:/{print $NF+0}'; }
# Total *used* RAM in GiB (the 'used' column, excludes buff/cache).
used_gb() { free -g | awk '/^Mem:/{print $3+0}'; }

# Hard memory cap. While total used RAM is at/above MEM_KILL_GB, kill the single
# heaviest running eds2leds worker and mark its chromosome "too intensive" so its
# remaining l-values stop and it can be retried later as one standalone process.
# Re-measures after each kill (kills one at a time) so we free just enough.
enforce_mem_cap() {
    while :; do
        local used; used=$(used_gb); used=${used:-0}
        (( used < MEM_KILL_GB )) && break

        # Heaviest eds2leds process (RSS-sorted). [e]ds2leds avoids self-matching grep.
        local line pid in_eds chrom
        line=$(ps -eo pid,rss,args --sort=-rss 2>/dev/null | grep -E '[e]ds2leds ' | head -n1)
        if [[ -z "$line" ]]; then
            echo "[MEMCAP] used=${used}GiB >= ${MEM_KILL_GB} but no eds2leds process to kill — cannot reclaim" >&2
            break
        fi
        pid=$(awk '{print $1}' <<<"$line")
        in_eds=$(grep -oE '\-i +[^ ]+' <<<"$line" | awk '{print $2}')
        chrom=$(basename "${in_eds:-unknown.eds}" .eds)

        mkdir -p "$TOO_INTENSIVE_DIR"
        # Marker tells the worker to stop before its next l-value; log is the retry list.
        touch "$TOO_INTENSIVE_DIR/${chrom}.abort"
        echo "$(date '+%Y-%m-%d %H:%M:%S') chr${chrom} (pid ${pid}) killed at used=${used}GiB" >> "$TOO_INTENSIVE_LOG"
        echo "[MEMCAP] used=${used}GiB >= ${MEM_KILL_GB} — killing chr${chrom} (pid ${pid}); marked too-intensive, retry later as a single process" >&2

        # SIGTERM the eds2leds run: it exits non-zero, so run_chrom_leds.sh's FAIL
        # branch removes the partial .leds/.seds, then it sees the marker and stops.
        kill -TERM "$pid" 2>/dev/null
        sleep 10  # give the kernel time to reclaim before re-measuring
    done
}

# ── Predict each chromosome's peak RAM ────────────────────────────────────────
# `eds2leds --estimate-memory` reads metadata only (no merging, no string reads) and
# prints RECOMMENDED_BUDGET_BYTES: index arrays + worst-case merge metadata + headroom.
# We estimate at the LARGEST l, which is the heaviest (more short contexts => more and
# bigger merge groups), and always from the raw EDS — for an incremental worker the real
# input is a smaller, already-merged l-EDS, so the raw-EDS number is an upper bound.
declare -A EST_BYTES
L_MAX=$(printf '%s\n' $L_VALUES | sort -n | tail -n1)

estimate_args=()
[[ -n "$BLOCK_SIZE_LEDS" ]] && estimate_args+=(--block-size "$BLOCK_SIZE_LEDS")

echo "--- Estimating peak memory per chromosome (l=${L_MAX}${BLOCK_SIZE_LEDS:+, block ${BLOCK_SIZE_LEDS}}) ---"
candidates=()
for eds_file in "$EDS_DIR"/*.eds; do
    [[ -f "$eds_file" ]] || { echo "No EDS files found in $EDS_DIR — run run_eds.sh first"; exit 1; }
    chrom=$(basename "$eds_file" .eds)
    seds_file="$EDS_DIR/${chrom}.seds"
    if [[ ! -f "$seds_file" ]]; then
        echo "[WARN] No SEDS for chr${chrom} ($seds_file), skipping"
        continue
    fi

    est_out=$(eds2leds -i "$eds_file" -s "$seds_file" -l "$L_MAX" \
                       "${estimate_args[@]}" --estimate-memory 2>/dev/null)
    est=$(grep '^RECOMMENDED_BUDGET_BYTES=' <<<"$est_out" | cut -d= -f2)
    sat=$(grep '^SATURATED='                <<<"$est_out" | cut -d= -f2)

    if [[ ! "$est" =~ ^[0-9]+$ ]] || (( est == 0 )); then
        # Estimation failed (unreadable input, tool error). Fall back to a crude
        # size-based proxy rather than treating the chromosome as free.
        fsize=$(stat -c %s "$eds_file" 2>/dev/null || echo 0)
        est=$(( fsize * 4 + (1 << 30) ))
        echo "[WARN] chr${chrom}: estimate failed, assuming $(( est / 1024/1024/1024 )) GiB from file size"
    elif [[ "$sat" == "1" ]]; then
        # Cartesian blow-up risk: the estimate overflowed. Force it to run alone.
        est=$(( $(budget_mib) > 0 ? $(budget_mib) * 1048576 : est ))
        echo "[WARN] chr${chrom}: unbounded merge estimate — will be scheduled alone"
    fi
    EST_BYTES["$chrom"]=$est
    printf '    chr%-4s %8d MiB predicted\n' "$chrom" "$(( est / 1048576 ))"
    candidates+=("$chrom")
done

# Heaviest first: better packing under a fixed budget, and the risky ones start while
# the machine is still empty.
IFS=$'\n' read -rd '' -a CHROM_ORDER < <(
    for c in "${candidates[@]}"; do printf '%s %s\n' "${EST_BYTES[$c]}" "$c"; done \
        | sort -rn | awk '{print $2}' && printf '\0')
echo "--- Launch order (heaviest first): ${CHROM_ORDER[*]} ---"

# Sum of predicted peaks (MiB) for workers still alive — the admission control input.
admitted_mib() {
    local total=0 c
    for c in "${!ADMITTED[@]}"; do
        worker_alive "$c" && total=$(( total + ADMITTED[$c] ))
    done
    echo $(( total / 1048576 ))
}

declare -A ADMITTED
spawned=()
for chrom in "${CHROM_ORDER[@]}"; do
    eds_file="$EDS_DIR/${chrom}.eds"
    seds_file="$EDS_DIR/${chrom}.seds"
    est=${EST_BYTES[$chrom]}
    est_mib=$(( est / 1048576 ))

    screen_name="${SCREEN_PREFIX}_${chrom}"

    # Already running (e.g. left over from a master run that was stopped)? Adopt it
    # rather than spawning a duplicate — a second concurrent worker for the same
    # chromosome would double its memory and race on the same output files. We just
    # add it to the wait set so the monitor loop below tracks it to completion.
    if worker_alive "$chrom"; then
        echo "[ADOPT] chr${chrom} already running as screen '$screen_name' — continuing with it"
        spawned+=("$screen_name")
        continue
    fi

    # Previously killed by the memory cap? Don't re-spawn into the same fate — leave
    # it for a deliberate single-process retry (rm the marker, then run
    # run_chrom_leds.sh directly). Adoption above still wins if it's somehow alive.
    if [[ -f "$TOO_INTENSIVE_DIR/${chrom}.abort" ]]; then
        echo "[SKIP] chr${chrom} marked too-intensive ($TOO_INTENSIVE_DIR/${chrom}.abort) — retry as a single process"
        continue
    fi

    # Wait for a slot: under the worker cap, enough RAM free right now, AND the
    # predicted peaks already in flight leave room for this one. The last condition is
    # the one that keeps two heavy chromosomes apart — free RAM alone is a lagging
    # signal, because eds2leds RSS ramps up over minutes.
    while :; do
        enforce_mem_cap  # kill runaway workers before they hit the ceiling
        refresh_limits   # pick up live changes to the control files
        workers=$(running_workers)
        free=$(free_gb); free=${free:-0}
        in_flight=$(admitted_mib)
        budget=$(budget_mib)

        fits_count=0; (( workers < MAX_PARALLEL )) && fits_count=1
        fits_free=0;  (( free >= MIN_FREE_GB ))    && fits_free=1
        fits_budget=1
        if (( budget > 0 )) && (( in_flight + est_mib > budget )); then
            # Nothing running and it still does not fit? Let it run alone rather than
            # deadlock — MEM_KILL_GB remains the backstop.
            (( workers == 0 )) && fits_budget=1 || fits_budget=0
        fi
        (( fits_count && fits_free && fits_budget )) && break

        echo "[$(date '+%H:%M:%S')] throttle chr${chrom} (needs ~${est_mib}MiB): workers=${workers}/${MAX_PARALLEL}, free=${free}GiB (need ${MIN_FREE_GB}), predicted in flight=${in_flight}/${budget}MiB — waiting"
        sleep 30
    done

    worker_log="$OUT_BASE/worker_logs/${chrom}.log"
    mkdir -p "$OUT_BASE/worker_logs"
    screen -dmS "$screen_name" bash -c "bash '$WORKER_SCRIPT' '$chrom' > '$worker_log' 2>&1"
    ADMITTED["$chrom"]=$est

    # A worker that dies on startup (missing tool, bad PATH, unreadable input) leaves
    # its screen gone within a second, which the monitor loop below cannot distinguish
    # from "finished". Confirm it is either still alive or actually did something.
    sleep 2
    if ! worker_alive "$chrom"; then
        if [[ -s "$worker_log" ]] && grep -qE 'START|SKIP' "$worker_log"; then
            : # ran and exited quickly — e.g. every l-value already present
        else
            echo "[ERROR] chr${chrom} worker exited immediately and produced no output." >&2
            echo "        See $worker_log — first lines:" >&2
            sed -n '1,5p' "$worker_log" 2>/dev/null | sed 's/^/        /' >&2
            echo "        Stopping: the remaining chromosomes would fail the same way." >&2
            exit 1
        fi
    fi
    echo "[SPAWN] screen '$screen_name' for chr${chrom} (~${est_mib}MiB predicted; workers now $(running_workers), free ${free}GiB, predicted in flight $(admitted_mib)/$(budget_mib)MiB)"
    spawned+=("$screen_name")
    # Brief stagger so a burst of launches doesn't all hit peak RAM in lockstep.
    sleep 5
done

if [[ ${#spawned[@]} -eq 0 ]]; then
    echo "Nothing to run."
    exit 0
fi

echo "--- Spawned ${#spawned[@]} screens, waiting for completion ---"

while true; do
    enforce_mem_cap  # hard ceiling also enforced while workers run
    ls_out=$(screen -ls 2>/dev/null)
    still_running=()
    for sname in "${spawned[@]}"; do
        # Anchor on the trailing whitespace after the session name so "eds2leds_1"
        # does not spuriously match "eds2leds_11", "eds2leds_12", ...
        if grep -qE "\.${sname}[[:space:]]" <<<"$ls_out"; then
            still_running+=("$sname")
        fi
    done

    if [[ ${#still_running[@]} -eq 0 ]]; then
        break
    fi

    echo "[$(date '+%H:%M:%S')] Still running: ${still_running[*]} (used $(used_gb)GiB, predicted in flight $(admitted_mib)MiB)"
    sleep 30
done

# Verify rather than assume: the monitor loop only knows that the screens are gone,
# which is also true when every worker died on startup.
missing=0; produced=0
for chrom in "${CHROM_ORDER[@]}"; do
    for l in $L_VALUES; do
        if [[ -s "$OUT_BASE/leds_l${l}/${chrom}.leds" ]]; then
            produced=$(( produced + 1 ))
        elif [[ -f "$TOO_INTENSIVE_DIR/${chrom}.abort" ]]; then
            :  # deliberately skipped, already reported
        else
            echo "[MISSING] chr${chrom} l=${l} — see $OUT_BASE/worker_logs/${chrom}.log" >&2
            missing=$(( missing + 1 ))
        fi
    done
done

echo "=== STAGE 2 FINISHED — $(date '+%Y-%m-%d %H:%M:%S') ==="
echo "    outputs present: ${produced} | missing: ${missing}"
echo "l-EDS files in: $OUT_BASE/leds_l*/"
if (( missing != 0 )); then
    echo "    NOT all outputs were produced — check the worker logs above." >&2
    screen -S "$MASTER_SCREEN" -X quit
    exit 1
fi
screen -S "$MASTER_SCREEN" -X quit
