#!/usr/bin/env python3
"""
diagnose_latency.py — compact latency-spike diagnosis for gateway JSONL logs
===========================================================================
Reads `metrics.jsonl` and `events.jsonl` from a capture folder and prints:

  1. aggregate timing/counter summary
  2. windows where read_to_publish_max/p95 exceeds a threshold
  3. a simple cause hint based on counters and timing fields

Usage:
  python diagnose_latency.py --log-dir ..\\logs\\latency_check
  python diagnose_latency.py --log-dir ..\\logs\\latency_check --threshold 300
  python diagnose_latency.py --log-dir ..\\logs\\latency_check --top 10
"""

from __future__ import annotations

import argparse
import json
import math
import os
import statistics
import sys
from collections import Counter
from typing import Any


RECOVERY_EVENT_TYPES = {
    "I2C_CONTROLLER_RESET",
    "PMBUS_BUS_RECOVERY",
    "PMBUS_BUS_RECOVERY_FAIL",
}

MQTT_EVENT_TYPES = {
    "MQTT_CONNECTED",
    "MQTT_DISCONNECTED",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Diagnose latency spikes from gateway metrics/events JSONL logs."
    )
    parser.add_argument(
        "--log-dir",
        required=True,
        help="Capture folder containing metrics.jsonl and optionally events.jsonl",
    )
    parser.add_argument(
        "--threshold",
        type=float,
        default=300.0,
        help="Spike threshold in milliseconds for read_to_publish_max or p95",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=8,
        help="How many spike windows to print",
    )
    return parser.parse_args()


def load_jsonl(path: str) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    if not os.path.exists(path):
        return rows

    with open(path, encoding="utf-8") as handle:
        for lineno, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError as exc:
                print(f"WARN: {path}:{lineno}: {exc}", file=sys.stderr)
                continue
            if isinstance(obj, dict):
                rows.append(obj)
    return rows


def get_nested(obj: dict[str, Any], *path: str, default: Any = 0) -> Any:
    cur: Any = obj
    for key in path:
        if not isinstance(cur, dict) or key not in cur:
            return default
        cur = cur[key]
    return cur


def to_float(value: Any) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return math.nan


def fmt_stat(values: list[float], unit: str = "ms") -> str:
    finite = [v for v in values if not math.isnan(v)]
    if not finite:
        return f"n/a {unit}"
    return (
        f"min/avg/max = {min(finite):.1f}/{statistics.mean(finite):.1f}/{max(finite):.1f} {unit}"
    )


def ts_key(row: dict[str, Any]) -> int:
    try:
        return int(row.get("ts_ms", 0))
    except (TypeError, ValueError):
        return 0


def select_events_for_window(events: list[dict[str, Any]], ts_ms: int, window_ms: int) -> list[dict[str, Any]]:
    if window_ms <= 0:
        return []
    start_ms = ts_ms - window_ms
    return [
        evt
        for evt in events
        if start_ms <= ts_key(evt) <= ts_ms
    ]


def diagnose_window(row: dict[str, Any]) -> str:
    timing = get_nested(row, "timing_ms", default={})
    counters = get_nested(row, "counters_delta", default={})

    mqtt_avg = to_float(get_nested(timing, "mqtt_publish_avg", default=math.nan))
    pmbus_avg = to_float(get_nested(timing, "pmbus_txn_avg", default=math.nan))
    ctrl_resets = int(get_nested(counters, "i2c_controller_resets", default=0))
    bus_recoveries = int(get_nested(counters, "i2c_bus_recoveries", default=0))
    mqtt_reconnects = int(get_nested(counters, "mqtt_reconnects", default=0))
    queue_drops = int(get_nested(counters, "queue_drops", default=0))
    buffer_depth = int(get_nested(row, "gauges", "buffer_depth_ram", default=0))

    if mqtt_reconnects > 0:
        return "likely network/reconnect-induced tail"
    if ctrl_resets > 0 or bus_recoveries > 0:
        return "likely I2C recovery-induced tail"
    if not math.isnan(mqtt_avg) and mqtt_avg >= 100.0:
        return "likely slow MQTT publish path"
    if not math.isnan(pmbus_avg) and pmbus_avg >= 30.0:
        return "likely slow PMBus transaction path"
    if queue_drops > 0:
        return "queue pressure observed"
    if buffer_depth > 0:
        return "buffering/backpressure observed"
    return "likely sporadic queue/scheduler jitter"


def main() -> int:
    args = parse_args()
    metrics_path = os.path.join(args.log_dir, "metrics.jsonl")
    events_path = os.path.join(args.log_dir, "events.jsonl")

    metrics_rows = load_jsonl(metrics_path)
    events_rows = load_jsonl(events_path)

    if not metrics_rows:
        print(f"ERROR: no metrics rows found in {metrics_path}", file=sys.stderr)
        return 1

    metrics_rows.sort(key=ts_key)
    events_rows.sort(key=ts_key)

    t_read_avg = [to_float(get_nested(r, "timing_ms", "read_to_publish_avg", default=math.nan)) for r in metrics_rows]
    t_read_p95 = [to_float(get_nested(r, "timing_ms", "read_to_publish_p95", default=math.nan)) for r in metrics_rows]
    t_read_max = [to_float(get_nested(r, "timing_ms", "read_to_publish_max", default=math.nan)) for r in metrics_rows]
    t_mqtt_avg = [to_float(get_nested(r, "timing_ms", "mqtt_publish_avg", default=math.nan)) for r in metrics_rows]
    t_pmbus_avg = [to_float(get_nested(r, "timing_ms", "pmbus_txn_avg", default=math.nan)) for r in metrics_rows]

    sum_ctrl_resets = sum(int(get_nested(r, "counters_delta", "i2c_controller_resets", default=0)) for r in metrics_rows)
    sum_bus_recoveries = sum(int(get_nested(r, "counters_delta", "i2c_bus_recoveries", default=0)) for r in metrics_rows)
    sum_mqtt_reconnects = sum(int(get_nested(r, "counters_delta", "mqtt_reconnects", default=0)) for r in metrics_rows)
    sum_queue_drops = sum(int(get_nested(r, "counters_delta", "queue_drops", default=0)) for r in metrics_rows)

    print("Latency summary")
    print(f"  windows               : {len(metrics_rows)}")
    print(f"  read_to_publish_avg   : {fmt_stat(t_read_avg)}")
    print(f"  read_to_publish_p95   : {fmt_stat(t_read_p95)}")
    print(f"  read_to_publish_max   : {fmt_stat(t_read_max)}")
    print(f"  mqtt_publish_avg      : {fmt_stat(t_mqtt_avg)}")
    print(f"  pmbus_txn_avg         : {fmt_stat(t_pmbus_avg)}")
    print(f"  i2c_controller_resets : {sum_ctrl_resets}")
    print(f"  i2c_bus_recoveries    : {sum_bus_recoveries}")
    print(f"  mqtt_reconnects       : {sum_mqtt_reconnects}")
    print(f"  queue_drops           : {sum_queue_drops}")

    event_counter = Counter(evt.get("type", "UNKNOWN") for evt in events_rows)
    interesting_event_types = sorted(set(RECOVERY_EVENT_TYPES | MQTT_EVENT_TYPES) & set(event_counter))
    if interesting_event_types:
        print("  notable events        :")
        for event_type in interesting_event_types:
            print(f"    {event_type}: {event_counter[event_type]}")

    spike_rows = [
        row for row in metrics_rows
        if (
            to_float(get_nested(row, "timing_ms", "read_to_publish_max", default=math.nan)) >= args.threshold
            or to_float(get_nested(row, "timing_ms", "read_to_publish_p95", default=math.nan)) >= args.threshold
        )
    ]

    if not spike_rows:
        print(f"\nNo spike windows above {args.threshold:.1f} ms.")
        return 0

    spike_rows.sort(
        key=lambda row: (
            max(
                to_float(get_nested(row, "timing_ms", "read_to_publish_max", default=math.nan)),
                to_float(get_nested(row, "timing_ms", "read_to_publish_p95", default=math.nan)),
            )
        ),
        reverse=True,
    )

    print(f"\nSpike windows (threshold {args.threshold:.1f} ms)")
    for index, row in enumerate(spike_rows[:args.top], 1):
        ts_ms = ts_key(row)
        window_ms = int(get_nested(row, "window_ms", default=0))
        timing = get_nested(row, "timing_ms", default={})
        counters = get_nested(row, "counters_delta", default={})
        related_events = select_events_for_window(events_rows, ts_ms, window_ms)
        event_types = [evt.get("type", "UNKNOWN") for evt in related_events]

        print(f"  [{index}] ts_ms={ts_ms} window_ms={window_ms}")
        print(
            "      r2p avg/p95/max    = "
            f"{to_float(get_nested(timing, 'read_to_publish_avg', default=math.nan)):.1f}/"
            f"{to_float(get_nested(timing, 'read_to_publish_p95', default=math.nan)):.1f}/"
            f"{to_float(get_nested(timing, 'read_to_publish_max', default=math.nan)):.1f} ms"
        )
        print(
            "      mqtt/pmbus avg     = "
            f"{to_float(get_nested(timing, 'mqtt_publish_avg', default=math.nan)):.1f}/"
            f"{to_float(get_nested(timing, 'pmbus_txn_avg', default=math.nan)):.1f} ms"
        )
        print(
            "      counters           = "
            f"ctrl_reset={int(get_nested(counters, 'i2c_controller_resets', default=0))} "
            f"bus_recovery={int(get_nested(counters, 'i2c_bus_recoveries', default=0))} "
            f"mqtt_reconnects={int(get_nested(counters, 'mqtt_reconnects', default=0))} "
            f"queue_drops={int(get_nested(counters, 'queue_drops', default=0))}"
        )
        if event_types:
            print(f"      events             = {', '.join(event_types)}")
        else:
            print("      events             = none in this window")
        print(f"      hint               = {diagnose_window(row)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
