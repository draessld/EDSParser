#!/usr/bin/env python3
"""bench_plot.py — Generate performance plots from a benchmark result CSV.

Usage:
    python3 bench_plot.py [CSV_PATH_OR_RESULTS_DIR]

If no argument is given, the newest *.csv in the script's own results/ directory is used.
Plots are saved to results/plots/<csv_stem>/ as PNG files.
"""

import re
import sys
from pathlib import Path

import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

# ---------------------------------------------------------------------------
# Colours — consistent across all plots
# ---------------------------------------------------------------------------

COLORS = {
    "cartesian":             "#2196F3",   # blue
    "linear":                "#FF9800",   # orange
    "eds2leds_cartesian":    "#2196F3",
    "eds2leds_linear":       "#FF9800",
    "genrandomeds":          "#4CAF50",   # green
    "edsparser-stats":       "#9C27B0",   # purple
    "edsparser-genpatterns": "#F44336",   # red
}

DISPLAY_NAMES = {
    "cartesian":             "cartesian",
    "linear":                "linear",
    "eds2leds_cartesian":    "eds2leds cartesian",
    "eds2leds_linear":       "eds2leds linear",
    "genrandomeds":          "genrandomeds",
    "edsparser-stats":       "edsparser-stats",
    "edsparser-genpatterns": "edsparser-genpatterns",
}

# ---------------------------------------------------------------------------
# Scenario classification
# ---------------------------------------------------------------------------

def classify(scenario: str) -> dict:
    """Return a dict of structured fields extracted from a scenario name string."""
    if m := re.match(r"variability_(\d+)_(\d+)_(cartesian|linear)$", scenario):
        # e.g. "variability_0_01_cartesian" → variability=0.01, mode="cartesian"
        return {
            "sweep": "variability",
            "variability": float(f"{m.group(1)}.{m.group(2)}"),
            "mode": m.group(3),
        }
    if m := re.match(r"context_l(\d+)_(cartesian|linear)$", scenario):
        return {"sweep": "context", "context_l": int(m.group(1)), "mode": m.group(2)}
    if m := re.match(r"path_count_min(\d+)_max(\d+)_linear$", scenario):
        return {
            "sweep": "path_count",
            "min_alt": int(m.group(1)),
            "max_alt": int(m.group(2)),
            "mode": "linear",
        }
    if m := re.match(r"genrandomeds_(\d+)mb$", scenario):
        return {"sweep": "size", "ref_size_mb": int(m.group(1)), "mode": "genrandomeds"}
    if m := re.match(r"eds2leds_(cartesian|linear)_(\d+)mb$", scenario):
        return {
            "sweep": "size",
            "ref_size_mb": int(m.group(2)),
            "mode": f"eds2leds_{m.group(1)}",
        }
    if m := re.match(r"edsparser_stats_(\d+)mb$", scenario):
        return {"sweep": "size", "ref_size_mb": int(m.group(1)), "mode": "edsparser-stats"}
    if m := re.match(r"edsparser_genpatterns_(\d+)mb$", scenario):
        return {"sweep": "size", "ref_size_mb": int(m.group(1)), "mode": "edsparser-genpatterns"}
    if m := re.match(r"memory_stability_(\d+)mb_(cartesian|linear)$", scenario):
        return {
            "sweep": "memory_stability",
            "ref_size_mb": int(m.group(1)),
            "mode": m.group(2),
        }
    return {"sweep": "other", "mode": "unknown"}


def load_and_classify(csv_path: Path) -> pd.DataFrame:
    """Load the CSV and append classification columns."""
    df = pd.read_csv(csv_path)
    classified = df["scenario"].apply(classify).apply(pd.Series)
    return pd.concat([df, classified], axis=1)


# ---------------------------------------------------------------------------
# Matplotlib style
# ---------------------------------------------------------------------------

def _apply_style():
    for style in ("seaborn-v0_8-whitegrid", "seaborn-whitegrid"):
        try:
            plt.style.use(style)
            return
        except OSError:
            pass
    # fallback: minimal tweaks on default style
    plt.rcParams.update({
        "axes.grid": True,
        "grid.alpha": 0.4,
        "axes.spines.top": False,
        "axes.spines.right": False,
    })


# ---------------------------------------------------------------------------
# Shared helper — draw a 2-panel (throughput | memory) figure for sweep data
# ---------------------------------------------------------------------------

def _sweep_figure(x_vals, series: dict, xlabel: str, title: str,
                  x_log: bool = False) -> plt.Figure:
    """
    series: {mode_key: (throughput_list, memory_list, tp_err_list, mem_err_list)}
    Error lists may be None (no error bars drawn).
    Returns a Figure with two subplots side by side.
    """
    fig, (ax_tp, ax_mem) = plt.subplots(1, 2, figsize=(10, 4))

    for mode, (tp, mem, tp_err, mem_err) in series.items():
        color = COLORS.get(mode, "#888888")
        label = DISPLAY_NAMES.get(mode, mode)
        kw = dict(color=color, marker="o", linewidth=1.8, markersize=6, label=label)
        ax_tp.errorbar(range(len(x_vals)), tp, yerr=tp_err, capsize=3, **kw)
        ax_mem.errorbar(range(len(x_vals)), mem, yerr=mem_err, capsize=3, **kw)

    for ax, ylabel in ((ax_tp, "Throughput (MB/s)"), (ax_mem, "Memory median (MB)")):
        ax.set_xlabel(xlabel)
        ax.set_ylabel(ylabel)
        ax.set_xticks(range(len(x_vals)))
        ax.set_xticklabels(x_vals)
        ax.legend(fontsize=8)
        if x_log:
            ax.set_xscale("log")

    fig.suptitle(title, fontsize=11, y=1.02)
    fig.tight_layout()
    return fig


# ---------------------------------------------------------------------------
# Individual plot functions
# ---------------------------------------------------------------------------

def plot_variability_sweep(df: pd.DataFrame, out_dir: Path) -> bool:
    sub = df[df["sweep"] == "variability"].copy()
    if sub.empty:
        return False

    var_vals = sorted(sub["variability"].unique())
    input_mb = sub["input_size_mb"].iloc[0]

    series = {}
    for mode in sub["mode"].unique():
        rows = sub[sub["mode"] == mode].sort_values("variability")
        tp     = [rows.loc[rows["variability"] == v, "throughput_median_mb_s"].mean() for v in var_vals]
        mem    = [rows.loc[rows["variability"] == v, "memory_median_mb"].mean()        for v in var_vals]
        tp_err = [rows.loc[rows["variability"] == v, "runtime_stddev_s"].mean()        for v in var_vals] \
                 if "runtime_stddev_s" in rows.columns else None
        mem_err = [rows.loc[rows["variability"] == v, "memory_stddev_mb"].mean()       for v in var_vals] \
                  if "memory_stddev_mb" in rows.columns else None
        series[mode] = (tp, mem, tp_err, mem_err)

    x_labels = [f"{v:.0%}" for v in var_vals]
    fig = _sweep_figure(
        x_labels, series,
        xlabel="Variability",
        title=f"EDS→l-EDS: throughput & memory vs variability  (input ≈{input_mb:.0f} MB)",
        x_log=False,
    )

    out = out_dir / "variability_sweep.png"
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return True


def plot_context_sweep(df: pd.DataFrame, out_dir: Path) -> bool:
    sub = df[df["sweep"] == "context"].copy()
    if sub.empty:
        return False

    ctx_vals = sorted(sub["context_l"].unique())
    input_mb = sub["input_size_mb"].iloc[0]

    series = {}
    for mode in sub["mode"].unique():
        rows = sub[sub["mode"] == mode].sort_values("context_l")
        tp      = [rows.loc[rows["context_l"] == l, "throughput_median_mb_s"].mean() for l in ctx_vals]
        mem     = [rows.loc[rows["context_l"] == l, "memory_median_mb"].mean()        for l in ctx_vals]
        tp_err  = [rows.loc[rows["context_l"] == l, "runtime_stddev_s"].mean()        for l in ctx_vals] \
                  if "runtime_stddev_s" in rows.columns else None
        mem_err = [rows.loc[rows["context_l"] == l, "memory_stddev_mb"].mean()        for l in ctx_vals] \
                  if "memory_stddev_mb" in rows.columns else None
        series[mode] = (tp, mem, tp_err, mem_err)

    x_labels = [str(l) for l in ctx_vals]
    fig = _sweep_figure(
        x_labels, series,
        xlabel="Context length  l",
        title=f"EDS→l-EDS: throughput & memory vs context length  (input ≈{input_mb:.0f} MB)",
    )

    out = out_dir / "context_length_sweep.png"
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return True


def plot_path_count_sweep(df: pd.DataFrame, out_dir: Path) -> bool:
    sub = df[df["sweep"] == "path_count"].copy()
    if sub.empty:
        return False

    alt_vals = sorted(sub["max_alt"].unique())
    input_mb = sub["input_size_mb"].iloc[0]

    rows = sub.sort_values("max_alt")
    tp      = [rows.loc[rows["max_alt"] == a, "throughput_median_mb_s"].mean() for a in alt_vals]
    mem     = [rows.loc[rows["max_alt"] == a, "memory_median_mb"].mean()        for a in alt_vals]
    tp_err  = [rows.loc[rows["max_alt"] == a, "runtime_stddev_s"].mean()        for a in alt_vals] \
              if "runtime_stddev_s" in rows.columns else None
    mem_err = [rows.loc[rows["max_alt"] == a, "memory_stddev_mb"].mean()        for a in alt_vals] \
              if "memory_stddev_mb" in rows.columns else None

    x_labels = [f"max {a} alt" for a in alt_vals]
    fig = _sweep_figure(
        x_labels, {"linear": (tp, mem, tp_err, mem_err)},
        xlabel="Max alternatives per degenerate symbol",
        title=f"eds2leds linear: throughput & memory vs path count  (input ≈{input_mb:.0f} MB)",
    )

    out = out_dir / "path_count_sweep.png"
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return True


def plot_size_sweep(df: pd.DataFrame, out_dir: Path) -> bool:
    sub = df[df["sweep"] == "size"].copy()
    if sub.empty:
        return False

    modes = sorted(sub["mode"].unique())
    fig, (ax_tp, ax_mem) = plt.subplots(1, 2, figsize=(10, 4))

    for mode in modes:
        rows = sub[sub["mode"] == mode].sort_values("ref_size_mb")
        if rows.empty:
            continue
        x = rows["ref_size_mb"].values
        tp  = rows["throughput_median_mb_s"].values
        mem = rows["memory_median_mb"].values
        color = COLORS.get(mode, "#888888")
        label = DISPLAY_NAMES.get(mode, mode)
        # Use markers only when there's a single point (no line to draw)
        kw = dict(color=color, marker="o", label=label,
                  linewidth=1.8 if len(x) > 1 else 0,
                  markersize=7 if len(x) == 1 else 5)
        ax_tp.plot(x, tp, **kw)
        ax_mem.plot(x, mem, **kw)

    for ax, ylabel in ((ax_tp, "Throughput (MB/s)"), (ax_mem, "Peak memory (MB)")):
        ax.set_xlabel("Input size (MB)")
        ax.set_ylabel(ylabel)
        ax.legend(fontsize=7, ncol=1)

    fig.suptitle("Tool throughput & memory vs input file size", fontsize=11, y=1.02)
    fig.tight_layout()

    out = out_dir / "size_sweep.png"
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return True


def plot_memory_stability(df: pd.DataFrame, out_dir: Path) -> bool:
    sub = df[df["sweep"] == "memory_stability"].copy()
    if sub.empty:
        return False

    MEMORY_LIMIT_MB = 500
    sizes = sorted(sub["ref_size_mb"].unique())

    fig, (ax_mem, ax_tp) = plt.subplots(1, 2, figsize=(10, 4))

    for mode in ("cartesian", "linear"):
        rows = sub[sub["mode"] == mode].sort_values("ref_size_mb")
        if rows.empty:
            continue
        x     = rows["ref_size_mb"].values
        mem   = rows["memory_median_mb"].values
        tp    = rows["throughput_median_mb_s"].values
        mem_err = rows["memory_stddev_mb"].values if "memory_stddev_mb" in rows.columns else None
        color = COLORS.get(mode, "#888888")
        label = DISPLAY_NAMES.get(mode, mode)
        kw = dict(color=color, marker="o", linewidth=1.8, markersize=6, label=label)
        ax_mem.errorbar(x, mem, yerr=mem_err, capsize=3, **kw)
        ax_tp.plot(x, tp, **kw)

    ax_mem.axhline(MEMORY_LIMIT_MB, color="#E53935", linewidth=1.4,
                   linestyle="--", label=f"{MEMORY_LIMIT_MB} MB limit")
    ax_mem.set_xlabel("Input size (MB)")
    ax_mem.set_ylabel("Peak memory (MB)")
    ax_mem.set_title("Peak RSS vs input size")
    ax_mem.legend(fontsize=8)

    ax_tp.set_xlabel("Input size (MB)")
    ax_tp.set_ylabel("Throughput (MB/s)")
    ax_tp.set_title("Throughput vs input size")
    ax_tp.legend(fontsize=8)

    fig.suptitle(
        f"Memory stability — large-file METADATA_ONLY streaming  "
        f"(10% var, --min-context 4, l=5)",
        fontsize=10, y=1.02,
    )
    fig.tight_layout()

    out = out_dir / "memory_stability.png"
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return True


def _label_for_summary(scenario: str) -> str:
    """Shorten a scenario name for the summary bar chart Y-axis."""
    replacements = [
        (r"variability_(\d+)_(\d+)_(cartesian|linear)",
         lambda m: f"{'cart' if m.group(3)=='cartesian' else 'lin'} v={float(m.group(1)+'.'+m.group(2)):.0%}"),
        (r"context_l(\d+)_(cartesian|linear)",
         lambda m: f"{'cart' if m.group(2)=='cartesian' else 'lin'} l={m.group(1)}"),
        (r"path_count_min(\d+)_max(\d+)_linear",
         lambda m: f"lin paths max={m.group(2)}"),
        (r"genrandomeds_(\d+)mb",
         lambda m: f"genrandomeds {m.group(1)}MB"),
        (r"eds2leds_(cartesian|linear)_(\d+)mb",
         lambda m: f"{'cart' if m.group(1)=='cartesian' else 'lin'} {m.group(2)}MB"),
        (r"edsparser_stats_(\d+)mb",
         lambda m: f"stats {m.group(1)}MB"),
        (r"edsparser_genpatterns_(\d+)mb",
         lambda m: f"genpatterns {m.group(1)}MB"),
        (r"memory_stability_(\d+)mb_(cartesian|linear)",
         lambda m: f"memstab {'cart' if m.group(2)=='cartesian' else 'lin'} {m.group(1)}MB"),
    ]
    for pattern, repl in replacements:
        if m := re.match(pattern, scenario):
            return repl(m)
    return scenario


def _color_for_scenario(scenario: str) -> str:
    if "cartesian" in scenario:
        return COLORS["cartesian"]
    if "linear" in scenario:
        return COLORS["linear"]
    if scenario.startswith("genrandomeds"):
        return COLORS["genrandomeds"]
    if "stats" in scenario:
        return COLORS["edsparser-stats"]
    if "genpatterns" in scenario:
        return COLORS["edsparser-genpatterns"]
    return "#888888"


def plot_summary(df: pd.DataFrame, out_dir: Path) -> bool:
    if df.empty:
        return False

    rows = df.copy()
    rows["label"] = rows["scenario"].apply(_label_for_summary)
    rows["bar_color"] = rows["scenario"].apply(_color_for_scenario)
    # Sort by sweep type then scenario so related entries cluster
    rows = rows.sort_values(["sweep", "scenario"], ascending=[True, True])

    n = len(rows)
    height = max(5.0, n * 0.35)
    fig, (ax_tp, ax_mem) = plt.subplots(1, 2, figsize=(14, height))

    y_pos = range(n)
    labels = rows["label"].tolist()
    colors = rows["bar_color"].tolist()

    tp_vals  = rows["throughput_median_mb_s"].tolist()
    mem_vals = rows["memory_median_mb"].tolist()
    tp_err   = rows["runtime_stddev_s"].tolist()  if "runtime_stddev_s"  in rows.columns else None
    mem_err  = rows["memory_stddev_mb"].tolist()  if "memory_stddev_mb"  in rows.columns else None

    ax_tp.barh(list(y_pos), tp_vals, xerr=tp_err, color=colors, edgecolor="none",
               error_kw=dict(ecolor="#555", capsize=3))
    ax_tp.set_yticks(list(y_pos))
    ax_tp.set_yticklabels(labels, fontsize=8)
    ax_tp.set_xlabel("Throughput median (MB/s)")
    ax_tp.set_title("Throughput")
    ax_tp.invert_yaxis()

    ax_mem.barh(list(y_pos), mem_vals, xerr=mem_err, color=colors, edgecolor="none",
                error_kw=dict(ecolor="#555", capsize=3))
    ax_mem.set_yticks(list(y_pos))
    ax_mem.set_yticklabels(labels, fontsize=8)
    ax_mem.set_xlabel("Memory median (MB)")
    ax_mem.set_title("Peak memory")
    ax_mem.invert_yaxis()

    # Legend
    from matplotlib.patches import Patch
    legend_items = [
        Patch(color=COLORS["cartesian"],             label="cartesian / eds2leds cart"),
        Patch(color=COLORS["linear"],                label="linear / eds2leds lin"),
        Patch(color=COLORS["genrandomeds"],          label="genrandomeds"),
        Patch(color=COLORS["edsparser-stats"],       label="edsparser-stats"),
        Patch(color=COLORS["edsparser-genpatterns"], label="edsparser-genpatterns"),
    ]
    fig.legend(handles=legend_items, loc="lower center", ncol=3,
               fontsize=8, bbox_to_anchor=(0.5, -0.06))

    ts = rows["timestamp"].iloc[0] if "timestamp" in rows.columns else ""
    fig.suptitle(f"Benchmark summary  {ts}", fontsize=11)
    fig.tight_layout()

    out = out_dir / "summary.png"
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return True


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def parse_args() -> Path:
    if len(sys.argv) > 2:
        print(f"Usage: bench_plot.py [CSV_PATH_OR_RESULTS_DIR]", file=sys.stderr)
        sys.exit(1)

    results_dir = Path(__file__).parent / "results"

    if len(sys.argv) == 1:
        # Auto-find newest CSV
        csvs = sorted(results_dir.glob("*.csv"))
        if not csvs:
            print(f"No CSV files found in {results_dir}", file=sys.stderr)
            sys.exit(1)
        return csvs[-1]

    target = Path(sys.argv[1])
    if target.is_dir():
        csvs = sorted(target.glob("*.csv"))
        if not csvs:
            print(f"No CSV files found in {target}", file=sys.stderr)
            sys.exit(1)
        return csvs[-1]

    if not target.exists():
        print(f"File not found: {target}", file=sys.stderr)
        sys.exit(1)
    return target


def main():
    _apply_style()

    csv_path = parse_args()
    print(f"Reading: {csv_path}")

    df = load_and_classify(csv_path)

    out_dir = csv_path.parent / "plots" / csv_path.stem
    out_dir.mkdir(parents=True, exist_ok=True)

    generated = []

    plots = [
        ("variability_sweep.png",    plot_variability_sweep),
        ("context_length_sweep.png", plot_context_sweep),
        ("path_count_sweep.png",     plot_path_count_sweep),
        ("size_sweep.png",           plot_size_sweep),
        ("memory_stability.png",     plot_memory_stability),
        ("summary.png",              plot_summary),
    ]

    for filename, fn in plots:
        ok = fn(df, out_dir)
        if ok:
            generated.append(filename)
        else:
            print(f"  (skipped {filename} — no matching data in CSV)")

    if generated:
        print(f"Plots written to: {out_dir}/")
        for f in generated:
            print(f"  {f}")
    else:
        print("No plots generated.")


if __name__ == "__main__":
    main()
