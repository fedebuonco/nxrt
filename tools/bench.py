#!/usr/bin/env python3
"""Benchmark a recorded nxrt run: the same route, the same measuring window, frame-time percentiles.

A route is a row of a game's `bench.tsv` (name, NXRT_INPUT script, window start in seconds, window length in
seconds, description). The run is a log with `[pace]` lines (`NXRT_LOG_PACE=1`); the tool selects the frames whose
start time falls in the window and reports period/cost percentiles and fps. It never runs a game itself: pair it with
whatever launched the run (a port, a test, a future bench runner), so the same parsing works for every variant.

    python3 tools/bench.py --routes games/<id>/bench.tsv --route zora-pan --log run.log
    python3 tools/bench.py --from 45 --secs 55 --log base=base.log --log fast=fast.log --json out.json

Percentiles use linear interpolation between the two nearest samples (p50 of an even count is the mean of the middle
pair). `--from`/`--secs` override the route; with no route and no override, the whole log is measured.
"""
import argparse
import json
import math
import os
import re
import sys

PACE = re.compile(
    r"\[pace\] period ([\d.]+) blocked ([\d.]+) busy ([\d.]+) cost ([\d.]+) ms"
    r"(?:.*?\bgpu ([\d.-]+))?"
)


def load_routes(path):
    """name -> {keys, start, secs, about}; comments and blank lines are skipped."""
    routes = {}
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            c = line.split("\t")
            if len(c) < 4:
                raise ValueError(f"{path}:{n}: a route needs name, keys, window start and window length")
            try:
                start, secs = float(c[2]), float(c[3])
            except ValueError:
                raise ValueError(f"{path}:{n}: window start/length must be numbers")
            if start < 0 or secs <= 0:
                raise ValueError(f"{path}:{n}: window start must be >= 0 and length > 0")
            routes[c[0]] = {"keys": c[1], "start": start, "secs": secs, "about": c[4] if len(c) > 4 else ""}
    return routes


def parse_log(path):
    """The [pace] frames of a log, in order: period, blocked, busy, cost (ms) and gpu when the line times it."""
    frames = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = PACE.search(line)
            if not m:
                continue
            frame = {"period": float(m.group(1)), "blocked": float(m.group(2)),
                     "busy": float(m.group(3)), "cost": float(m.group(4))}
            if m.group(5) not in (None, "-1", "-1.0"):
                frame["gpu"] = float(m.group(5))
            frames.append(frame)
    return frames


def select(frames, start=0.0, secs=None):
    """Frames whose start time is in [start, start+secs); secs None: to the end."""
    out, t = [], 0.0
    lo, hi = start * 1000.0, None if secs is None else (start + secs) * 1000.0
    for f in frames:
        if t >= lo and (hi is None or t < hi):
            out.append(f)
        t += f["period"]
    return out


def percentile(values, p):
    if not values:
        return None
    v = sorted(values)
    if len(v) == 1:
        return v[0]
    k = (len(v) - 1) * p / 100.0
    f, c = math.floor(k), math.ceil(k)
    return v[f] if f == c else v[f] + (v[c] - v[f]) * (k - f)


def summarize(frames):
    if not frames:
        return {"frames": 0}
    period = [f["period"] for f in frames]
    cost = [f["cost"] for f in frames]
    s = {
        "frames": len(frames),
        "seconds": round(sum(period) / 1000.0, 3),
        "fps": round(1000.0 / (sum(period) / len(period)), 2) if sum(period) else None,
    }
    for name, vals in (("period", period), ("cost", cost)):
        for p in (50, 95, 99):
            s[f"{name}_p{p}"] = round(percentile(vals, p), 2)
        s[f"{name}_max"] = round(max(vals), 2)
    gpu = [f["gpu"] for f in frames if "gpu" in f]
    if gpu:
        s["gpu_p50"] = round(percentile(gpu, 50), 2)
        s["gpu_p95"] = round(percentile(gpu, 95), 2)
    return s


def label_path(spec):
    label, sep, path = spec.partition("=")
    if not sep:
        label, path = os.path.splitext(os.path.basename(spec))[0], spec
    return label, path


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--routes", help="a game's bench.tsv")
    ap.add_argument("--route", help="a route name in --routes")
    ap.add_argument("--from", dest="start", type=float, help="window start (s), overrides the route")
    ap.add_argument("--secs", type=float, help="window length (s), overrides the route")
    ap.add_argument("--log", action="append", default=[], metavar="[LABEL=]PATH", help="a run's log (repeatable)")
    ap.add_argument("--json", help="write the results as JSON here")
    a = ap.parse_args(argv)
    if not a.log:
        ap.error("--log is required")

    start, secs = a.start, a.secs
    if a.routes or a.route:
        if not (a.routes and a.route):
            ap.error("--routes and --route go together")
        routes = load_routes(a.routes)
        if a.route not in routes:
            ap.error(f"no route {a.route!r} in {a.routes} (have: {', '.join(sorted(routes))})")
        r = routes[a.route]
        if start is None:
            start = r["start"]
        if secs is None:
            secs = r["secs"]
    if start is None:
        start = 0.0

    rows = []
    for spec in a.log:
        label, path = label_path(spec)
        frames = parse_log(path)
        chosen = select(frames, start, secs)
        s = summarize(chosen)
        s["label"], s["log"] = label, path
        rows.append(s)
        if not chosen:
            print(f"{label}: no [pace] frames in the window (log has {len(frames)})", file=sys.stderr)

    print(f"{'run':20} {'frames':>6} {'fps':>6} {'period p50/p95/p99':>20} {'cost p50/p95/p99':>20}")
    for s in rows:
        if not s["frames"]:
            print(f"{s['label']:20} {'0':>6}")
            continue
        per = f"{s['period_p50']}/{s['period_p95']}/{s['period_p99']}"
        cost = f"{s['cost_p50']}/{s['cost_p95']}/{s['cost_p99']}"
        print(f"{s['label']:20} {s['frames']:>6} {s['fps']:>6} {per:>20} {cost:>20}")

    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump({"window": {"start": start, "secs": secs}, "runs": rows}, f, indent=2)
            f.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
