#!/usr/bin/env python3
"""
capture.py — PMBus-MQTT Edge Gateway log capture
=================================================
Subscribes to all gateway topics and appends each message to a JSONL file.

Output files (in --out-dir, default: ./logs/<timestamp>/):
  telemetry.jsonl
  status.jsonl
  metrics.jsonl
  events.jsonl

Usage:
  python capture.py
  python capture.py --host 192.168.1.2 --port 1883 --gw gw01
  python capture.py --out-dir logs/exp1_latency
  python capture.py --duration 300          # stop after 300 s
"""

import argparse
import json
import os
import sys
import time
import signal
import datetime

try:
    import paho.mqtt.client as mqtt
    from paho.mqtt.enums import CallbackAPIVersion
except ImportError:
    print("ERROR: paho-mqtt not installed.  Run:  pip install paho-mqtt")
    sys.exit(1)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def parse_args():
    p = argparse.ArgumentParser(description="Capture PMBus-MQTT gateway messages to JSONL files.")
    p.add_argument("--host",     default="192.168.1.2",  help="MQTT broker host")
    p.add_argument("--port",     default=1883, type=int, help="MQTT broker port")
    p.add_argument("--gw",       default="+",            help="Gateway ID filter (default: all)")
    p.add_argument("--out-dir",  default=None,           help="Output directory (default: logs/<timestamp>)")
    p.add_argument("--duration", default=0, type=float,  help="Stop after N seconds (0 = run forever)")
    p.add_argument("--quiet",    action="store_true",    help="Suppress per-message console output")
    return p.parse_args()


# ---------------------------------------------------------------------------
# State
# ---------------------------------------------------------------------------
class Capture:
    def __init__(self, out_dir: str, quiet: bool):
        os.makedirs(out_dir, exist_ok=True)
        self.out_dir  = out_dir
        self.quiet    = quiet
        self.counts   = {"telemetry": 0, "status": 0, "metrics": 0, "events": 0}
        self._files   = {
            "telemetry": open(os.path.join(out_dir, "telemetry.jsonl"), "a", encoding="utf-8"),
            "status":    open(os.path.join(out_dir, "status.jsonl"),    "a", encoding="utf-8"),
            "metrics":   open(os.path.join(out_dir, "metrics.jsonl"),   "a", encoding="utf-8"),
            "events":    open(os.path.join(out_dir, "events.jsonl"),    "a", encoding="utf-8"),
        }
        self.start_time = time.time()
        print(f"[CAP] Writing to {out_dir}/")
        for k in self._files:
            print(f"[CAP]   {k}.jsonl")

    def record(self, topic: str, payload_bytes: bytes):
        """Parse, classify, and write one message."""
        recv_ms = int(time.time() * 1000)
        try:
            obj = json.loads(payload_bytes.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError) as e:
            print(f"[CAP] WARN: bad JSON on {topic}: {e}", file=sys.stderr)
            return

        # Inject capture-time metadata
        obj["_topic"]   = topic
        obj["_recv_ms"] = recv_ms

        kind = self._classify(topic)
        line = json.dumps(obj, separators=(",", ":")) + "\n"
        self._files[kind].write(line)
        self._files[kind].flush()
        self.counts[kind] += 1

        if not self.quiet:
            elapsed = time.time() - self.start_time
            print(f"[{kind:9s}] t={elapsed:7.1f}s  topic={topic}  bytes={len(payload_bytes)}")

    @staticmethod
    def _classify(topic: str) -> str:
        if "/telemetry" in topic: return "telemetry"
        if "/status"    in topic: return "status"
        if "/metrics"   in topic: return "metrics"
        if "/events"    in topic: return "events"
        return "events"  # fallback

    def close(self):
        for f in self._files.values():
            f.close()
        total   = sum(self.counts.values())
        elapsed = time.time() - self.start_time
        print(f"\n[CAP] Capture complete — {elapsed:.1f} s")
        print(f"[CAP]   telemetry : {self.counts['telemetry']:6d}")
        print(f"[CAP]   status    : {self.counts['status']:6d}")
        print(f"[CAP]   metrics   : {self.counts['metrics']:6d}")
        print(f"[CAP]   events    : {self.counts['events']:6d}")
        print(f"[CAP]   total     : {total:6d}")


# ---------------------------------------------------------------------------
# MQTT callbacks
# ---------------------------------------------------------------------------
def make_client(args, capture: Capture) -> mqtt.Client:
    client = mqtt.Client(CallbackAPIVersion.VERSION1, client_id="pmbus-capture", protocol=mqtt.MQTTv311)

    def on_connect(c, userdata, flags, rc):
        if rc != 0:
            print(f"[CAP] Connect failed: rc={rc}", file=sys.stderr)
            return
        gw     = args.gw
        topics = [
            (f"pmbus/{gw}/dev/+/telemetry", 0),
            (f"pmbus/{gw}/dev/+/status",    0),
            (f"pmbus/{gw}/metrics",         0),
            (f"pmbus/{gw}/events",          0),
        ]
        c.subscribe(topics)
        print(f"[CAP] Connected to {args.host}:{args.port}")
        for t, _ in topics:
            print(f"[CAP]   subscribed: {t}")

    def on_disconnect(c, userdata, rc):
        if rc != 0:
            print(f"[CAP] Unexpected disconnect rc={rc} — will reconnect", file=sys.stderr)

    def on_message(c, userdata, msg):
        capture.record(msg.topic, msg.payload)

    client.on_connect    = on_connect
    client.on_disconnect = on_disconnect
    client.on_message    = on_message
    return client


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    args = parse_args()

    if args.out_dir is None:
        ts      = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        out_dir = os.path.join("logs", ts)
    else:
        out_dir = args.out_dir

    capture = Capture(out_dir, args.quiet)

    stop = [False]
    def _sig(signum, frame):
        stop[0] = True
    signal.signal(signal.SIGINT, _sig)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, _sig)

    client   = make_client(args, capture)
    client.connect(args.host, args.port, keepalive=60)
    client.loop_start()

    deadline = (time.time() + args.duration) if args.duration > 0 else None

    try:
        while not stop[0]:
            if deadline and time.time() >= deadline:
                print(f"\n[CAP] Duration {args.duration:.0f}s reached — stopping")
                break
            time.sleep(0.1)
    finally:
        client.loop_stop()
        client.disconnect()
        capture.close()


if __name__ == "__main__":
    main()
