#!/usr/bin/env python3
"""
plot.py — PMBus-MQTT Edge Gateway experiment plots
===================================================
Reads JSONL log files produced by capture.py and generates plots for the
4 thesis experiments.

Usage:
  python plot.py --log-dir logs/20260222_153000
  python plot.py --log-dir logs/exp1_latency   --out-dir figures/exp1
  python plot.py --log-dir logs/exp3_offline   --offline-start 60 --offline-end 180

Output (PNG files in --out-dir, default: <log-dir>/figures/):
  latency.png       — read_to_publish p95/avg/max vs time
  buffer.png        — buffer_depth_ram vs time (offline window shaded)
  errors.png        — pmbus_reads_fail / retries / pec_fail vs time
  throughput.png    — telemetry msgs/s and pmbus cmds/s vs time
  telemetry.png     — vin/vout/iout/temp vs time (from telemetry.jsonl)

Requirements:
  pip install matplotlib pandas
"""

import argparse
import json
import os
import sys

try:
    import matplotlib
    matplotlib.use("Agg")          # non-interactive backend — no display needed
    import matplotlib.pyplot as plt
    import matplotlib.patches as mpatches
    from matplotlib.ticker import MaxNLocator
except ImportError:
    print("ERROR: matplotlib not installed.  Run:  pip install matplotlib")
    sys.exit(1)

try:
    import pandas as pd
except ImportError:
    print("ERROR: pandas not installed.  Run:  pip install pandas")
    sys.exit(1)


# ---------------------------------------------------------------------------
# Style
# ---------------------------------------------------------------------------
plt.rcParams.update({
    "figure.dpi":       150,
    "figure.figsize":   (10, 4),
    "axes.grid":        True,
    "grid.alpha":       0.35,
    "lines.linewidth":  1.4,
    "font.size":        10,
})

COLORS = {
    "p95":    "#e63946",
    "avg":    "#457b9d",
    "max":    "#f4a261",
    "fail":   "#e63946",
    "retry":  "#f4a261",
    "pec":    "#a8dadc",
    "buf":    "#2a9d8f",
    "msgs":   "#457b9d",
    "cmds":   "#2a9d8f",
    "vin":    "#264653",
    "vout":   "#e9c46a",
    "iout":   "#f4a261",
    "temp":   "#e63946",
    "pout":   "#2a9d8f",
}


# ---------------------------------------------------------------------------
# JSONL loader
# ---------------------------------------------------------------------------
def load_jsonl(path: str) -> pd.DataFrame:
    """Load a JSONL file; return empty DataFrame if file missing/empty."""
    if not os.path.exists(path):
        return pd.DataFrame()
    rows = []
    with open(path, encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as e:
                print(f"  WARN: {path}:{lineno}: {e}", file=sys.stderr)
    if not rows:
        return pd.DataFrame()
    df = pd.json_normalize(rows)
    return df


def elapsed_s(df: pd.DataFrame, ts_col: str = "ts_ms", t0=None) -> pd.Series:
    """Convert ts_ms column to elapsed seconds from the provided or first record."""
    if t0 is None:
        t0 = df[ts_col].iloc[0]
    return (df[ts_col] - t0) / 1000.0


# ---------------------------------------------------------------------------
# Plot helpers
# ---------------------------------------------------------------------------
def save(fig, path: str):
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)
    print(f"  saved: {path}")


def shade_offline(ax, offline_start: float, offline_end: float):
    """Add a shaded region for the offline window."""
    if offline_start is not None and offline_end is not None:
        ax.axvspan(offline_start, offline_end, alpha=0.15, color="red", label="offline window")


# ---------------------------------------------------------------------------
# Plot 1 — Latency (metrics.jsonl)
# ---------------------------------------------------------------------------
def plot_latency(df_m: pd.DataFrame, out_dir: str, offline_start, offline_end):
    if df_m.empty:
        print("  SKIP latency.png — no metrics data")
        return

    t = elapsed_s(df_m)

    # timing_ms values come in as strings from the firmware ("12.3")
    def to_float(col):
        if col not in df_m.columns:
            return pd.Series([float("nan")] * len(df_m))
        return pd.to_numeric(df_m[col], errors="coerce")

    p95 = to_float("timing_ms.read_to_publish_p95")
    avg = to_float("timing_ms.read_to_publish_avg")
    mx  = to_float("timing_ms.read_to_publish_max")

    fig, ax = plt.subplots()
    ax.plot(t, p95, color=COLORS["p95"], label="p95")
    ax.plot(t, avg, color=COLORS["avg"], label="avg")
    ax.plot(t, mx,  color=COLORS["max"], label="max", alpha=0.5, linestyle="--")
    shade_offline(ax, offline_start, offline_end)
    ax.set_xlabel("Elapsed time (s)")
    ax.set_ylabel("Read→publish latency (ms)")
    ax.set_title("End-to-end latency: read → MQTT publish")
    ax.legend()
    save(fig, os.path.join(out_dir, "latency.png"))


# ---------------------------------------------------------------------------
# Plot 2 — Buffer depth (metrics.jsonl)
# ---------------------------------------------------------------------------
def plot_buffer(df_m: pd.DataFrame, out_dir: str, offline_start, offline_end):
    if df_m.empty:
        print("  SKIP buffer.png — no metrics data")
        return

    t   = elapsed_s(df_m)
    buf = pd.to_numeric(df_m.get("gauges.buffer_depth_ram", pd.Series(dtype=float)), errors="coerce")

    fig, ax = plt.subplots()
    ax.fill_between(t, buf, alpha=0.4, color=COLORS["buf"])
    ax.plot(t, buf, color=COLORS["buf"], label="RAM buffer depth (records)")
    shade_offline(ax, offline_start, offline_end)

    # Mark drops if present
    dropped_col = "counters_delta.buffer_dropped"
    if dropped_col in df_m.columns:
        drops = pd.to_numeric(df_m[dropped_col], errors="coerce")
        mask  = drops > 0
        if mask.any():
            ax.scatter(t[mask], buf[mask], color="red", zorder=5, label="records dropped", s=30)

    ax.set_xlabel("Elapsed time (s)")
    ax.set_ylabel("Buffer depth (records)")
    ax.set_title("Store-and-forward buffer depth")
    ax.yaxis.set_major_locator(MaxNLocator(integer=True))
    ax.legend()
    save(fig, os.path.join(out_dir, "buffer.png"))


# ---------------------------------------------------------------------------
# Plot 3 — Errors & retries (metrics.jsonl)
# ---------------------------------------------------------------------------
def plot_errors(df_m: pd.DataFrame, out_dir: str, offline_start, offline_end):
    if df_m.empty:
        print("  SKIP errors.png — no metrics data")
        return

    t    = elapsed_s(df_m)

    def delta(col):
        if col not in df_m.columns:
            return pd.Series([0.0] * len(df_m))
        return pd.to_numeric(df_m[col], errors="coerce").fillna(0)

    fail = delta("counters_delta.pmbus_reads_fail")
    rty  = delta("counters_delta.pmbus_retries")
    pec  = delta("counters_delta.pmbus_crc_pec_fail")
    nack = delta("counters_delta.pmbus_nack")
    tmo  = delta("counters_delta.pmbus_timeouts")

    fig, (ax1, ax2) = plt.subplots(2, 1, sharex=True, figsize=(10, 6))

    # Dynamic bar width: 80% of median interval (in seconds), fallback 1.8
    bar_w = 1.8
    if len(t) > 1:
        diffs = t.diff().dropna()
        if not diffs.empty:
            bar_w = float(diffs.median()) * 0.8

    ax1.bar(t, fail + pec + nack + tmo, color=COLORS["fail"], alpha=0.7, label="reads_fail+pec+nack+timeout", width=bar_w)
    ax1.set_ylabel("Error count (per window)")
    ax1.set_title("PMBus errors per metrics window")
    ax1.legend(fontsize=8)
    shade_offline(ax1, offline_start, offline_end)

    ax2.bar(t, rty, color=COLORS["retry"], alpha=0.7, label="retries", width=bar_w)
    ax2.set_xlabel("Elapsed time (s)")
    ax2.set_ylabel("Retry count (per window)")
    ax2.set_title("PMBus retries per metrics window")
    ax2.legend(fontsize=8)
    shade_offline(ax2, offline_start, offline_end)

    save(fig, os.path.join(out_dir, "errors.png"))


# ---------------------------------------------------------------------------
# Plot 4 — Throughput (metrics.jsonl)
# ---------------------------------------------------------------------------
def plot_throughput(df_m: pd.DataFrame, out_dir: str, offline_start, offline_end):
    if df_m.empty:
        print("  SKIP throughput.png — no metrics data")
        return

    t = elapsed_s(df_m)

    # rates are bare numbers in the firmware JSON (not strings)
    def rate(col):
        if col not in df_m.columns:
            return pd.Series([float("nan")] * len(df_m))
        return pd.to_numeric(df_m[col], errors="coerce")

    msgs = rate("rates.telemetry_msgs_per_s")
    cmds = rate("rates.pmbus_cmds_per_s")

    fig, ax = plt.subplots()
    ax.plot(t, msgs, color=COLORS["msgs"], label="telemetry msgs/s")
    ax.plot(t, cmds, color=COLORS["cmds"], label="PMBus cmds/s",  linestyle="--")
    shade_offline(ax, offline_start, offline_end)
    ax.set_xlabel("Elapsed time (s)")
    ax.set_ylabel("Rate (per second)")
    ax.set_title("Gateway throughput")
    ax.legend()
    save(fig, os.path.join(out_dir, "throughput.png"))


# ---------------------------------------------------------------------------
# Plot 5 — Telemetry values (telemetry.jsonl)
# ---------------------------------------------------------------------------
def plot_telemetry(df_t: pd.DataFrame, out_dir: str, offline_start, offline_end):
    if df_t.empty:
        print("  SKIP telemetry.png — no telemetry data")
        return

    df_t = df_t.sort_values("ts_ms").reset_index(drop=True)
    t0 = df_t["ts_ms"].iloc[0]

    def col(df, name):
        if name not in df.columns:
            return pd.Series([float("nan")] * len(df), index=df.index)
        return pd.to_numeric(df[name], errors="coerce")

    def target_label(df):
        addr = str(df["addr"].dropna().iloc[0]) if "addr" in df.columns and df["addr"].notna().any() else "telemetry"
        if "label" in df.columns and df["label"].notna().any():
            return f"{addr} ({df['label'].dropna().iloc[0]})"
        return addr

    if "addr" in df_t.columns and df_t["addr"].notna().any():
        groups = [(target_label(df_g), df_g.sort_values("ts_ms").reset_index(drop=True))
                  for _, df_g in df_t.groupby("addr", sort=True)]
    else:
        groups = [(target_label(df_t), df_t)]

    target_colors = list(plt.get_cmap("tab10").colors)

    metric_specs = [
        (0, 0, "v.vin",  "VIN",   "V"),
        (0, 1, "v.vout", "VOUT",  "V"),
        (1, 0, "a.iout", "IOUT",  "A"),
        (1, 1, "c.temp1","TEMP1", "\u00b0C"),
        (2, 0, "w.pout", "POUT",  "W"),
    ]

    fig, axes = plt.subplots(3, 2, figsize=(12, 9), sharex=True)
    axes[2, 1].axis("off")  # empty cell

    for row, col_idx, field, title, ylabel in metric_specs:
        ax = axes[row, col_idx]
        for idx, (label, df_g) in enumerate(groups):
            t = elapsed_s(df_g, t0=t0)
            ax.plot(
                t,
                col(df_g, field),
                color=target_colors[idx % len(target_colors)],
                linewidth=1.2,
                label=label,
            )

        shade_offline(ax, offline_start, offline_end)
        ax.set_ylabel(ylabel)
        ax.set_title(title)
        if len(groups) > 1:
            ax.legend(fontsize=8)

    for ax in axes[2, :]:
        ax.set_xlabel("Elapsed time (s)")

    if len(groups) == 1:
        title = f"PMBus telemetry — {groups[0][0]}"
    else:
        title = "PMBus telemetry — per target"
    fig.suptitle(title, fontsize=11)
    save(fig, os.path.join(out_dir, "telemetry.png"))


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def parse_args():
    p = argparse.ArgumentParser(description="Generate plots from PMBus-MQTT capture logs.")
    p.add_argument("--log-dir",       default="logs",   help="Directory containing *.jsonl files")
    p.add_argument("--out-dir",       default=None,     help="Output directory for PNG files (default: <log-dir>/figures)")
    p.add_argument("--offline-start", default=None, type=float, help="Offline window start (seconds from t=0)")
    p.add_argument("--offline-end",   default=None, type=float, help="Offline window end (seconds from t=0)")
    p.add_argument("--no-telemetry",  action="store_true", help="Skip telemetry.png (large files)")
    return p.parse_args()


def main():
    args    = parse_args()
    log_dir = args.log_dir

    if args.out_dir:
        out_dir = args.out_dir
    else:
        out_dir = os.path.join(log_dir, "figures")

    os.makedirs(out_dir, exist_ok=True)
    print(f"[PLOT] Log dir : {log_dir}")
    print(f"[PLOT] Out dir : {out_dir}")

    df_m = load_jsonl(os.path.join(log_dir, "metrics.jsonl"))
    df_t = load_jsonl(os.path.join(log_dir, "telemetry.jsonl"))

    # Drop the first metrics snapshot whose window_ms == 0 (boot artefact).
    if not df_m.empty and "window_ms" in df_m.columns:
        df_m = df_m[pd.to_numeric(df_m["window_ms"], errors="coerce") > 0].reset_index(drop=True)

    print(f"[PLOT] Loaded  : {len(df_m)} metrics records, {len(df_t)} telemetry records")

    os_  = args.offline_start
    oe_  = args.offline_end

    plot_latency(   df_m, out_dir, os_, oe_)
    plot_buffer(    df_m, out_dir, os_, oe_)
    plot_errors(    df_m, out_dir, os_, oe_)
    plot_throughput(df_m, out_dir, os_, oe_)
    if not args.no_telemetry:
        plot_telemetry(df_t, out_dir, os_, oe_)

    print("[PLOT] Done.")


if __name__ == "__main__":
    main()
