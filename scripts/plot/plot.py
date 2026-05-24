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
  latency.png       — per-window read_to_publish p95/avg/max vs time
  latency_breakdown.png — per-window additive average latency decomposition
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

OFFLINE_START_EVENT_TYPES = {"GATEWAY_UNEXPECTED_DISCONNECT", "MQTT_DISCONNECTED"}
OFFLINE_END_EVENT_TYPES = {"MQTT_CONNECTED"}


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


def _series_numeric_value(row: pd.Series, key: str):
    """Return a single numeric value from a normalized JSON row or None."""
    if key not in row.index:
        return None
    value = pd.to_numeric(pd.Series([row[key]]), errors="coerce").iloc[0]
    if pd.isna(value):
        return None
    return float(value)


def infer_axis_offset_ms(df_ref: pd.DataFrame, axis_col: str):
    """Estimate axis_col - _recv_ms offset for ts-based axes."""
    if axis_col == "_recv_ms":
        return 0.0
    if axis_col != "ts_ms":
        return None
    if df_ref.empty or "ts_ms" not in df_ref.columns or "_recv_ms" not in df_ref.columns:
        return None

    ts = pd.to_numeric(df_ref["ts_ms"], errors="coerce")
    recv = pd.to_numeric(df_ref["_recv_ms"], errors="coerce")
    mask = ts.notna() & recv.notna()
    if "time_synced" in df_ref.columns:
        sync = df_ref["time_synced"].fillna(False).astype(bool)
        if sync.any():
            mask &= sync
    diffs = (ts - recv)[mask].dropna()
    if diffs.empty:
        return None
    return float(diffs.median())


def _event_axis_ms(row: pd.Series, axis_col: str, offset_ms):
    """Project an event row onto the requested axis."""
    if axis_col == "_recv_ms":
        return _series_numeric_value(row, "_recv_ms")

    if axis_col == "ts_ms":
        ts_ms = _series_numeric_value(row, "ts_ms")
        time_synced = bool(row.get("time_synced", False))
        if ts_ms is not None and time_synced:
            return ts_ms
        recv_ms = _series_numeric_value(row, "_recv_ms")
        if recv_ms is not None and offset_ms is not None:
            return recv_ms + offset_ms
        return None

    value = _series_numeric_value(row, axis_col)
    if value is not None:
        return value
    recv_ms = _series_numeric_value(row, "_recv_ms")
    if recv_ms is not None and offset_ms is not None:
        return recv_ms + offset_ms
    return None


def infer_offline_window_from_events(df_e: pd.DataFrame, df_ref: pd.DataFrame, axis_col: str, label: str):
    """Infer the first disconnect→reconnect window in seconds for the given plot axis."""
    if df_e.empty or df_ref.empty or axis_col not in df_ref.columns or "type" not in df_e.columns:
        return None, None

    sort_col = "_recv_ms" if "_recv_ms" in df_e.columns else "ts_ms"
    if sort_col not in df_e.columns:
        return None, None

    df_sorted = df_e.sort_values(sort_col).reset_index(drop=True)
    start_rows = df_sorted[df_sorted["type"].isin(OFFLINE_START_EVENT_TYPES)]
    if start_rows.empty:
        return None, None

    start_idx = start_rows.index[0]
    end_rows = df_sorted.iloc[start_idx + 1:]
    end_rows = end_rows[end_rows["type"].isin(OFFLINE_END_EVENT_TYPES)]
    if end_rows.empty:
        return None, None

    offset_ms = infer_axis_offset_ms(df_ref, axis_col)
    start_ms = _event_axis_ms(df_sorted.iloc[start_idx], axis_col, offset_ms)
    end_ms = _event_axis_ms(end_rows.iloc[0], axis_col, offset_ms)
    if start_ms is None or end_ms is None:
        return None, None

    axis_series = pd.to_numeric(df_ref[axis_col], errors="coerce").dropna()
    if axis_series.empty:
        return None, None
    t0 = float(axis_series.iloc[0])
    offline_start = (start_ms - t0) / 1000.0
    offline_end = (end_ms - t0) / 1000.0
    print(f"[PLOT] Auto offline window for {label}: {offline_start:.3f}s .. {offline_end:.3f}s")
    return offline_start, offline_end


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

    # Newer firmware reports per-window sample counts.  Windows with no
    # telemetry samples encode latency stats as 0 for a fixed JSON schema;
    # mask those values so plots do not show fake zero-latency points.
    sample_col = "timing_samples.read_to_publish_window"
    if sample_col in df_m.columns:
        samples = pd.to_numeric(df_m[sample_col], errors="coerce")
        has_samples = samples > 0
        p95 = p95.where(has_samples)
        avg = avg.where(has_samples)
        mx = mx.where(has_samples)

    fig, ax = plt.subplots()
    ax.plot(t, p95, color=COLORS["p95"], label="p95")
    ax.plot(t, avg, color=COLORS["avg"], label="avg")
    ax.plot(t, mx,  color=COLORS["max"], label="max", alpha=0.5, linestyle="--")
    shade_offline(ax, offline_start, offline_end)
    ax.set_xlabel("Elapsed time (s)")
    ax.set_ylabel("Read→publish latency (ms)")
    ax.set_title("End-to-end latency per metrics window: read → MQTT publish")
    ax.legend()
    save(fig, os.path.join(out_dir, "latency.png"))


def plot_latency_breakdown(df_m: pd.DataFrame, out_dir: str, offline_start, offline_end):
    if df_m.empty:
        print("  SKIP latency_breakdown.png — no metrics data")
        return

    t = elapsed_s(df_m)

    def to_float(col):
        if col not in df_m.columns:
            return pd.Series([float("nan")] * len(df_m))
        return pd.to_numeric(df_m[col], errors="coerce")

    total = to_float("timing_ms.read_to_publish_avg")
    before_pub = to_float("timing_ms.telemetry_before_publish_avg")
    telem_pub = to_float("timing_ms.telemetry_publish_avg")

    sample_col = "timing_samples.read_to_publish_window"
    if sample_col in df_m.columns:
        samples = pd.to_numeric(df_m[sample_col], errors="coerce")
        has_samples = samples > 0
        total = total.where(has_samples)
        before_pub = before_pub.where(has_samples)
        telem_pub = telem_pub.where(has_samples)

    if before_pub.isna().all() or telem_pub.isna().all():
        print("  SKIP latency_breakdown.png — telemetry breakdown fields not present")
        return

    fig, ax = plt.subplots()
    ax.plot(t, total, color=COLORS["avg"], label="e2e avg")
    ax.plot(t, before_pub, color=COLORS["p95"], label="before publish avg")
    ax.plot(t, telem_pub, color=COLORS["max"], label="telemetry publish avg")
    shade_offline(ax, offline_start, offline_end)
    ax.set_xlabel("Elapsed time (s)")
    ax.set_ylabel("Latency (ms)")
    ax.set_title("Average telemetry latency decomposition: e2e = before publish + publish")
    ax.legend()
    save(fig, os.path.join(out_dir, "latency_breakdown.png"))


# ---------------------------------------------------------------------------
# Plot 2 — Buffer depth (metrics.jsonl)
# ---------------------------------------------------------------------------
def plot_buffer(df_m: pd.DataFrame, out_dir: str, offline_start, offline_end):
    if df_m.empty:
        print("  SKIP buffer.png — no metrics data")
        return

    t     = elapsed_s(df_m)
    ram   = pd.to_numeric(df_m.get("gauges.buffer_depth_ram", pd.Series(dtype=float)), errors="coerce")
    flash = pd.to_numeric(df_m.get("gauges.buffer_depth_flash", pd.Series(dtype=float)), errors="coerce")

    fig, ax = plt.subplots()
    ax.fill_between(t, ram, alpha=0.25, color=COLORS["buf"])
    ax.plot(t, ram, color=COLORS["buf"], label="RAM depth snapshot")
    if not flash.isna().all():
        ax.plot(t, flash, color=COLORS["max"], linestyle="--", label="Flash depth snapshot")
    shade_offline(ax, offline_start, offline_end)

    # Mark drops if present
    dropped_col = "counters_delta.buffer_dropped"
    if dropped_col in df_m.columns:
        drops = pd.to_numeric(df_m[dropped_col], errors="coerce")
        mask  = drops > 0
        if mask.any():
            ax.scatter(t[mask], ram[mask], color="red", zorder=5, label="records dropped", s=30)

    if ram.fillna(0).eq(0).all() and flash.fillna(0).eq(0).all():
        enq = pd.to_numeric(df_m.get("counters_delta.buffer_enqueued", pd.Series(dtype=float)), errors="coerce").fillna(0)
        deq = pd.to_numeric(df_m.get("counters_delta.buffer_dequeued", pd.Series(dtype=float)), errors="coerce").fillna(0)
        if (enq > 0).any() or (deq > 0).any():
            print("  INFO buffer.png - depth gauges stay at zero; buffering activity happened within the window and drained before each metrics snapshot")

    ax.set_xlabel("Elapsed time (s)")
    ax.set_ylabel("Buffer depth (records)")
    ax.set_title("Store-and-forward buffer depth (snapshot gauges)")
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
def _telemetry_col(df: pd.DataFrame, name: str) -> pd.Series:
    if name not in df.columns:
        return pd.Series([float("nan")] * len(df), index=df.index)
    return pd.to_numeric(df[name], errors="coerce")


def _telemetry_target_label(df: pd.DataFrame) -> str:
    addr = str(df["addr"].dropna().iloc[0]) if "addr" in df.columns and df["addr"].notna().any() else "telemetry"
    if "label" in df.columns and df["label"].notna().any():
        return f"{addr} ({df['label'].dropna().iloc[0]})"
    return addr


def _telemetry_has_provenance(df_t: pd.DataFrame) -> bool:
    if "boot_count" not in df_t.columns or "sample_monotonic_ms" not in df_t.columns:
        return False
    boot = pd.to_numeric(df_t["boot_count"], errors="coerce")
    sample = pd.to_numeric(df_t["sample_monotonic_ms"], errors="coerce")
    return (boot.notna() & sample.notna()).any()


def _telemetry_device_time_segments(df_t: pd.DataFrame):
    boot = pd.to_numeric(df_t["boot_count"], errors="coerce")
    sample = pd.to_numeric(df_t["sample_monotonic_ms"], errors="coerce")
    mask = boot.notna() & sample.notna()
    if not mask.any():
        return []

    df = df_t.loc[mask].copy()
    df["_boot"] = boot.loc[mask].astype(int)
    df["_sample_ms"] = sample.loc[mask].astype(float)

    if "addr" in df.columns and df["addr"].notna().any():
        grouped = [df_g.copy() for _, df_g in df.groupby("addr", sort=True)]
    else:
        grouped = [df]

    segments = []
    gap_s = 5.0
    for df_addr in grouped:
        base_label = _telemetry_target_label(df_addr)
        offset_s = 0.0
        multi_boot = df_addr["_boot"].nunique() > 1
        for boot_id, df_boot in df_addr.groupby("_boot", sort=True):
            sort_cols = ["_sample_ms"]
            if "seq" in df_boot.columns:
                sort_cols.append("seq")
            df_boot = df_boot.sort_values(sort_cols).reset_index(drop=True).copy()
            local_t = (df_boot["_sample_ms"] - df_boot["_sample_ms"].iloc[0]) / 1000.0
            df_boot["_plot_t"] = local_t + offset_s
            label = f"{base_label} boot#{boot_id}" if multi_boot else base_label
            segments.append((label, df_boot))
            offset_s = float(df_boot["_plot_t"].iloc[-1]) + gap_s

    return segments


def _telemetry_capture_segments(df_t: pd.DataFrame):
    df = df_t.sort_values("_recv_ms").reset_index(drop=True).copy()

    if "boot_count" not in df.columns:
        if "addr" in df.columns and df["addr"].notna().any():
            return [(_telemetry_target_label(df_g), df_g.sort_values("_recv_ms").reset_index(drop=True))
                    for _, df_g in df.groupby("addr", sort=True)]
        return [(_telemetry_target_label(df), df)]

    boot = pd.to_numeric(df["boot_count"], errors="coerce")
    if boot.notna().sum() == 0:
        if "addr" in df.columns and df["addr"].notna().any():
            return [(_telemetry_target_label(df_g), df_g.sort_values("_recv_ms").reset_index(drop=True))
                    for _, df_g in df.groupby("addr", sort=True)]
        return [(_telemetry_target_label(df), df)]

    df["_boot"] = boot
    if "addr" in df.columns and df["addr"].notna().any():
        grouped = [df_g.copy() for _, df_g in df.groupby("addr", sort=True)]
    else:
        grouped = [df]

    segments = []
    for df_addr in grouped:
        base_label = _telemetry_target_label(df_addr)
        multi_boot = df_addr["_boot"].dropna().nunique() > 1
        for boot_id, df_boot in df_addr.groupby("_boot", sort=True, dropna=False):
            df_boot = df_boot.sort_values("_recv_ms").reset_index(drop=True).copy()
            if pd.isna(boot_id) or not multi_boot:
                label = base_label
            else:
                label = f"{base_label} boot#{int(boot_id)}"
            segments.append((label, df_boot))

    return segments


def plot_telemetry(df_t: pd.DataFrame, out_dir: str, offline_start, offline_end):
    if df_t.empty:
        print("  SKIP telemetry.png — no telemetry data")
        return

    provenance_mode = _telemetry_has_provenance(df_t)
    if provenance_mode:
        print("  INFO telemetry.png - using boot_count + sample_monotonic_ms segmentation")
        groups = _telemetry_device_time_segments(df_t)
        if not groups:
            print("  SKIP telemetry.png - telemetry provenance fields are present but unusable")
            return
    else:
        if "time_synced" in df_t.columns:
            sync_state = df_t["time_synced"].dropna().astype(bool)
            if not sync_state.empty and sync_state.any() and (~sync_state).any():
                unsynced_count = int((~sync_state).sum())
                print(f"  INFO telemetry.png - dropping {unsynced_count} unsynced boot samples")
                df_t = df_t[df_t["time_synced"] == True].copy()
                if df_t.empty:
                    print("  SKIP telemetry.png - only unsynced telemetry samples present")
                    return

        df_t = df_t.sort_values("ts_ms").reset_index(drop=True)
        t0 = df_t["ts_ms"].iloc[0]
        if "addr" in df_t.columns and df_t["addr"].notna().any():
            groups = [(_telemetry_target_label(df_g), df_g.sort_values("ts_ms").reset_index(drop=True))
                      for _, df_g in df_t.groupby("addr", sort=True)]
        else:
            groups = [(_telemetry_target_label(df_t), df_t)]

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
            if provenance_mode:
                t = pd.to_numeric(df_g["_plot_t"], errors="coerce")
            else:
                t = elapsed_s(df_g, t0=t0)
            ax.plot(
                t,
                _telemetry_col(df_g, field),
                color=target_colors[idx % len(target_colors)],
                linewidth=1.2,
                label=label,
            )

        if not provenance_mode:
            shade_offline(ax, offline_start, offline_end)
        ax.set_ylabel(ylabel)
        ax.set_title(title)
        if len(groups) > 1:
            ax.legend(fontsize=8)

    for ax in axes[2, :]:
        ax.set_xlabel("Elapsed device time (s, segmented by boot)" if provenance_mode
                      else "Elapsed time (s)")

    if len(groups) == 1:
        title = f"PMBus telemetry by device time — {groups[0][0]}" if provenance_mode else f"PMBus telemetry — {groups[0][0]}"
    else:
        title = "PMBus telemetry by device time — per target / boot" if provenance_mode else "PMBus telemetry — per target"
    fig.suptitle(title, fontsize=11)
    save(fig, os.path.join(out_dir, "telemetry.png"))


def plot_telemetry_capture_time(df_t: pd.DataFrame, out_dir: str, offline_start, offline_end):
    """Plot telemetry against capture-time to visualize outages/reboots cleanly."""
    if df_t.empty:
        print("  SKIP telemetry_capture_time.png - no telemetry data")
        return
    if "_recv_ms" not in df_t.columns:
        print("  SKIP telemetry_capture_time.png - no _recv_ms field present")
        return

    df_t = df_t.sort_values("_recv_ms").reset_index(drop=True)
    t0 = df_t["_recv_ms"].iloc[0]
    groups = _telemetry_capture_segments(df_t)

    target_colors = list(plt.get_cmap("tab10").colors)
    metric_specs = [
        (0, 0, "v.vin",  "VIN",   "V"),
        (0, 1, "v.vout", "VOUT",  "V"),
        (1, 0, "a.iout", "IOUT",  "A"),
        (1, 1, "c.temp1","TEMP1", "\u00b0C"),
        (2, 0, "w.pout", "POUT",  "W"),
    ]

    fig, axes = plt.subplots(3, 2, figsize=(12, 9), sharex=True)
    axes[2, 1].axis("off")

    for row, col_idx, field, title, ylabel in metric_specs:
        ax = axes[row, col_idx]
        for idx, (label, df_g) in enumerate(groups):
            t = elapsed_s(df_g, ts_col="_recv_ms", t0=t0)
            ax.plot(
                t,
                _telemetry_col(df_g, field),
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
        ax.set_xlabel("Elapsed capture time (s)")

    if len(groups) == 1:
        title = f"PMBus telemetry by capture time - {groups[0][0]}"
    else:
        title = "PMBus telemetry by capture time - per target"
    fig.suptitle(title, fontsize=11)
    save(fig, os.path.join(out_dir, "telemetry_capture_time.png"))


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


def resolve_offline_window(df_e: pd.DataFrame, df_ref: pd.DataFrame, axis_col: str, label: str, manual_start, manual_end):
    """Resolve offline shading bounds for one plot family."""
    if manual_start is not None and manual_end is not None:
        print(f"[PLOT] Manual offline window for {label}: {manual_start:.3f}s .. {manual_end:.3f}s")
        return manual_start, manual_end
    return infer_offline_window_from_events(df_e, df_ref, axis_col, label)


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
    df_e = load_jsonl(os.path.join(log_dir, "events.jsonl"))

    # Drop the first metrics snapshot whose window_ms == 0 (boot artefact).
    if not df_m.empty and "window_ms" in df_m.columns:
        df_m = df_m[pd.to_numeric(df_m["window_ms"], errors="coerce") > 0].reset_index(drop=True)

    print(f"[PLOT] Loaded  : {len(df_m)} metrics records, {len(df_t)} telemetry records, {len(df_e)} events")

    metrics_os, metrics_oe = resolve_offline_window(
        df_e, df_m, "ts_ms", "metrics", args.offline_start, args.offline_end
    )
    telemetry_os, telemetry_oe = resolve_offline_window(
        df_e, df_t, "ts_ms", "telemetry", args.offline_start, args.offline_end
    )
    capture_os, capture_oe = resolve_offline_window(
        df_e, df_t, "_recv_ms", "telemetry-capture", args.offline_start, args.offline_end
    )

    plot_latency(   df_m, out_dir, metrics_os, metrics_oe)
    plot_latency_breakdown(df_m, out_dir, metrics_os, metrics_oe)
    plot_buffer(    df_m, out_dir, metrics_os, metrics_oe)
    plot_errors(    df_m, out_dir, metrics_os, metrics_oe)
    plot_throughput(df_m, out_dir, metrics_os, metrics_oe)
    if not args.no_telemetry:
        plot_telemetry(df_t, out_dir, telemetry_os, telemetry_oe)
        plot_telemetry_capture_time(df_t, out_dir, capture_os, capture_oe)

    print("[PLOT] Done.")


if __name__ == "__main__":
    main()
