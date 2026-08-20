#!/usr/bin/env python3
"""
Join the linear and cartesian arms of an l-EDS sweep into one comparison table.

The two merges see the same input and pick the same merge groups; they differ only
in what they keep inside a group:

  linear     intersects the sources of the alternatives it concatenates, so a
             combination survives only if some sample actually carries it. The
             l-EDS is this dataset, realised at context length l.
  cartesian  keeps every combination, so it also spells out recombinants that were
             never sequenced. It needs no sources and writes no .seds.

So the ratio cartesian/linear is what the dataset's own realisation buys: the share
of the l-EDS that is combinations nobody carries. The runtime and peak-RSS ratios
are the other side of the trade — what the source bookkeeping that removes them
costs, plus the .seds that has to be carried alongside the .leds to keep meaning it.

Usage:
    compare_merge_modes.py <results_dir> [<results_dir>...] [--output merge_modes.csv]

    results_dir   a bundle written by collect_results.sh, e.g. results/tb_p500

Reads, per bundle:
    eds2leds/stats/leds_l<N>.csv        linear l-EDS stats   (from edsparser-stats)
    eds2leds/stats/leds_l<N>_cart.csv   cartesian l-EDS stats
    eds2leds/l<N>[_cart]/*.out          transformation logs  (runtime, peak RSS)
    vcf2eds/stats/all_stats.csv         the input EDS, as the l=0 baseline
    other/all_sizes_bytes.txt           `du -ab`, for the .seds sizes
    other/leds_runs.tsv                 outcomes, so a run killed at the memory cap
                                        is reported as such instead of going missing
"""

import argparse
import csv
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

PERF_RE = re.compile(r"\[Performance\]\s+Runtime:\s+([\d.]+)s\s+\|\s+Peak Memory:\s+([\d.]+)\s+MB")
ITER_RE = re.compile(r"Converged after (\d+) iterations")


def read_csv_rows(path: Path) -> List[Dict[str, str]]:
    if not path.is_file():
        return []
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def num(row: Optional[Dict[str, str]], key: str, cast=int):
    """Field lookup that tolerates the column being absent or blank."""
    if not row:
        return None
    v = row.get(key, "")
    if v is None or v == "":
        return None
    try:
        return cast(v)
    except ValueError:
        return None


def parse_log(path: Path) -> Dict[str, Optional[float]]:
    """Runtime, peak RSS and iteration count from one eds2leds log."""
    out: Dict[str, Optional[float]] = {"runtime_s": None, "peak_mb": None, "iterations": None}
    if not path.is_file():
        return out
    text = path.read_text(errors="replace")
    if (m := PERF_RE.search(text)):
        out["runtime_s"] = float(m.group(1))
        out["peak_mb"] = float(m.group(2))
    if (m := ITER_RE.search(text)):
        out["iterations"] = int(m.group(1))
    return out


def find_log(bundle: Path, l: str, mode: str, chrom: str) -> Path:
    """collect_results.sh keeps the runner's file names: <chrom>_l<N>.out."""
    d = bundle / "eds2leds" / (f"l{l}_cart" if mode == "cartesian" else f"l{l}")
    exact = d / f"{chrom}_l{l}.out"
    if exact.is_file():
        return exact
    hits = sorted(d.glob(f"{chrom}*.out"))
    return hits[0] if hits else exact


def read_sizes(bundle: Path) -> Dict[str, int]:
    """`du -ab` output as {relative path: bytes}."""
    sizes: Dict[str, int] = {}
    p = bundle / "other" / "all_sizes_bytes.txt"
    if not p.is_file():
        return sizes
    for line in p.read_text(errors="replace").splitlines():
        parts = line.split("\t")
        if len(parts) == 2 and parts[0].isdigit():
            sizes[parts[1].lstrip("./")] = int(parts[0])
    return sizes


def read_runs(bundle: Path) -> Dict[Tuple[str, str, str], str]:
    """{(mode, l, chrom): status} — the only record of runs that produced no file."""
    runs: Dict[Tuple[str, str, str], str] = {}
    p = bundle / "other" / "leds_runs.tsv"
    if not p.is_file():
        return runs
    with p.open(newline="") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            key = (row.get("mode", ""), row.get("l", ""), row.get("chrom", ""))
            # Later attempts supersede earlier ones (the runner appends, never rewrites).
            runs[key] = row.get("status", "")
    return runs


def stats_by_chrom(path: Path) -> Dict[str, Dict[str, str]]:
    return {r["chrom"]: r for r in read_csv_rows(path) if r.get("chrom")}


def eds_baseline(bundle: Path) -> Dict[str, Dict[str, str]]:
    """The input EDS stats, keyed by chromosome (derived from the `file` column)."""
    out = {}
    for r in read_csv_rows(bundle / "vcf2eds" / "stats" / "all_stats.csv"):
        stem = Path(r.get("file", "")).stem
        if stem:
            out[stem] = r
    return out


def ratio(a, b) -> Optional[float]:
    if a is None or b in (None, 0):
        return None
    return a / b


def collect(bundle: Path) -> List[Dict]:
    name = bundle.name
    stats_dir = bundle / "eds2leds" / "stats"
    if not stats_dir.is_dir():
        print(f"warning: {name}: no eds2leds/stats — skipping", file=sys.stderr)
        return []

    sizes = read_sizes(bundle)
    runs = read_runs(bundle)
    eds = eds_baseline(bundle)

    l_values = sorted(
        {p.stem[len("leds_l"):].removesuffix("_cart") for p in stats_dir.glob("leds_l*.csv")},
        key=lambda s: (not s.isdigit(), int(s) if s.isdigit() else s),
    )

    rows: List[Dict] = []
    for l in l_values:
        lin = stats_by_chrom(stats_dir / f"leds_l{l}.csv")
        car = stats_by_chrom(stats_dir / f"leds_l{l}_cart.csv")
        # A chromosome the cartesian arm never finished has no stats row but is still
        # worth a row here, carrying its status — that is the headline result for
        # dense data at high l, not a gap.
        chroms = sorted(set(lin) | set(car) |
                        {c for (m, ll, c) in runs if ll == l})
        for c in chroms:
            lr, cr = lin.get(c), car.get(c)
            llog = parse_log(find_log(bundle, l, "linear", c))
            clog = parse_log(find_log(bundle, l, "cartesian", c))

            lin_strings, cart_strings = num(lr, "m_strings"), num(cr, "m_strings")
            lin_leds, cart_leds = num(lr, "file_size_bytes"), num(cr, "file_size_bytes")
            lin_seds = sizes.get(f"leds_l{l}/{c}.seds")
            lin_total = None if lin_leds is None else lin_leds + (lin_seds or 0)

            rows.append({
                "dataset": name,
                "chrom": c,
                "l": l,
                "num_paths": num(lr, "num_paths") or num(cr, "num_paths"),
                "lin_status": runs.get(("linear", l, c), "ok" if lr else "missing"),
                "cart_status": runs.get(("cartesian", l, c), "ok" if cr else "missing"),

                # Baseline: the EDS the two arms were both built from.
                "eds_strings": num(eds.get(c), "m_strings"),
                "eds_bytes": num(eds.get(c), "file_size_bytes"),
                "eds_ctx_avg": num(eds.get(c), "context_avg", float),

                # What each arm produced.
                "lin_strings": lin_strings,
                "cart_strings": cart_strings,
                "string_ratio": ratio(cart_strings, lin_strings),
                "unobserved_strings": None if None in (lin_strings, cart_strings)
                                      else cart_strings - lin_strings,
                "unobserved_frac": None if None in (lin_strings, cart_strings) or not cart_strings
                                   else (cart_strings - lin_strings) / cart_strings,

                # Disk. Linear only means anything with its .seds, so charge it for both.
                "lin_leds_bytes": lin_leds,
                "lin_seds_bytes": lin_seds,
                "lin_total_bytes": lin_total,
                "cart_leds_bytes": cart_leds,
                "leds_byte_ratio": ratio(cart_leds, lin_leds),
                "total_byte_ratio": ratio(cart_leds, lin_total),

                # Cost of the source bookkeeping.
                "lin_runtime_s": llog["runtime_s"],
                "cart_runtime_s": clog["runtime_s"],
                "runtime_ratio": ratio(llog["runtime_s"], clog["runtime_s"]),
                "lin_peak_mb": llog["peak_mb"],
                "cart_peak_mb": clog["peak_mb"],
                "mem_ratio": ratio(llog["peak_mb"], clog["peak_mb"]),
                "lin_iterations": llog["iterations"],
                "cart_iterations": clog["iterations"],
            })
    return rows


FIELDS = [
    "dataset", "chrom", "l", "num_paths", "lin_status", "cart_status",
    "eds_strings", "eds_bytes", "eds_ctx_avg",
    "lin_strings", "cart_strings", "string_ratio", "unobserved_strings", "unobserved_frac",
    "lin_leds_bytes", "lin_seds_bytes", "lin_total_bytes", "cart_leds_bytes",
    "leds_byte_ratio", "total_byte_ratio",
    "lin_runtime_s", "cart_runtime_s", "runtime_ratio",
    "lin_peak_mb", "cart_peak_mb", "mem_ratio",
    "lin_iterations", "cart_iterations",
]


def fmt(v, spec="") -> str:
    return "-" if v is None else (f"{v:{spec}}" if spec else str(v))


def print_table(rows: List[Dict]) -> None:
    hdr = (f"{'dataset':<18} {'l':>4} {'paths':>6} {'lin str':>12} {'cart str':>12} "
           f"{'cart/lin':>9} {'unobs':>7} {'lin s':>8} {'cart s':>8} {'lin MB':>9} {'cart MB':>9}")
    print(f"\n{hdr}\n{'-' * len(hdr)}")
    for r in rows:
        note = ""
        for arm, key in (("lin", "lin_status"), ("cart", "cart_status")):
            if r[key] not in ("ok", "missing"):
                note += f"  [{arm}: {r[key]}]"
        print(f"{r['dataset']:<18} {r['l']:>4} {fmt(r['num_paths']):>6} "
              f"{fmt(r['lin_strings'], ',') :>12} {fmt(r['cart_strings'], ','):>12} "
              f"{fmt(r['string_ratio'], '.2f'):>9} {fmt(r['unobserved_frac'], '.1%'):>7} "
              f"{fmt(r['lin_runtime_s'], '.1f'):>8} {fmt(r['cart_runtime_s'], '.1f'):>8} "
              f"{fmt(r['lin_peak_mb'], '.0f'):>9} {fmt(r['cart_peak_mb'], '.0f'):>9}{note}")

    done = [r for r in rows if r["string_ratio"] is not None]
    if done:
        worst = max(done, key=lambda r: r["string_ratio"])
        print(f"\n{len(done)}/{len(rows)} pairs complete. "
              f"Largest over-generation: {worst['dataset']} l={worst['l']} "
              f"at {worst['string_ratio']:.2f}x "
              f"({worst['unobserved_frac']:.1%} of strings unobserved).")
    incomplete = [r for r in rows if r["cart_status"] not in ("ok", "missing")]
    if incomplete:
        print(f"{len(incomplete)} cartesian run(s) did not finish — "
              f"the explosion is itself the measurement; see leds_runs.tsv.")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Compare the linear and cartesian l-EDS merges over collected results.")
    ap.add_argument("results_dir", type=Path, nargs="+",
                    help="bundle(s) written by collect_results.sh, e.g. results/tb_p500")
    ap.add_argument("-o", "--output", type=Path, help="write the joined table to this CSV")
    args = ap.parse_args()

    rows: List[Dict] = []
    for d in args.results_dir:
        if not d.is_dir():
            print(f"error: no such results directory: {d}", file=sys.stderr)
            return 1
        rows.extend(collect(d))

    if not rows:
        print("No l-EDS stats found. Run the sweep with MODES=\"linear cartesian\" first.",
              file=sys.stderr)
        return 1

    rows.sort(key=lambda r: (r["dataset"], r["chrom"],
                             int(r["l"]) if r["l"].isdigit() else 0))
    print_table(rows)

    if args.output:
        with args.output.open("w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=FIELDS)
            w.writeheader()
            w.writerows(rows)
        print(f"\nWrote {len(rows)} rows to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
