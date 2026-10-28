#!/usr/bin/env python3
"""One-command game smoke test: run a port to a screenshot and check the log.

    python3 tools/smoke.py fe3h            # 900 frames, Metal 4 on
    python3 tools/smoke.py eow --metal3 --frames 600
    python3 tools/smoke.py --app ... --data ... --frames 300

It checks: exit 0, a non-black screenshot, and no crash / GPU command-buffer error.
"""
import argparse
import os
import subprocess
import sys
import tempfile

GAMES = {
    "fe3h": {
        "app": "/Volumes/FE3HDev/fe3h/build-macoscore/fe3hrecomp",
        "data": "/Volumes/FE3HDev/fe3h",
    },
    "eow": {
        "app": "/Volumes/PortableSSD/eow-macoscore/build-macoscore/eowrecomp",
        "data": "/Volumes/PortableSSD/EoW Recomp game",
    },
}
BAD_LOG = ("[crash]", "signal 11", "command buffer error", "FATAL")
MIN_PNG = 50_000  # a black 1280x720 frame is ~17 KB; a real scene is hundreds of KB


def check_log(text):
    return [line for line in text.splitlines() if any(b in line for b in BAD_LOG)]


def run(app, data, frames, metal4, shot, log_path):
    env = dict(os.environ)
    env.update({
        "NXRT_DATA_ROOT": data,
        "NXRT_NO_LAUNCHER": "1",
        "NXRT_WINDOW_SHOT": str(frames),
        "NXRT_SHOT_PATH": shot,
        "NXRT_EXIT_AFTER_SHOT": "1",
        "NXRT_METAL4": "1" if metal4 else "0",
    })
    with open(log_path, "w") as log:
        p = subprocess.run([app], env=env, stdout=log, stderr=subprocess.STDOUT, timeout=300)
    text = open(log_path, errors="replace").read()
    bad = check_log(text)
    size = os.path.getsize(shot) if os.path.exists(shot) else 0
    if p.returncode != 0:
        return False, f"exit {p.returncode}; see {log_path}"
    if bad:
        return False, f"{len(bad)} bad log line(s): {bad[0]}"
    if size < MIN_PNG:
        return False, f"screenshot {size} bytes (< {MIN_PNG}); see {log_path}"
    return True, f"{size} bytes, {frames} frames, Metal {'4' if metal4 else '3'}"


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("game", nargs="?", choices=sorted(GAMES))
    ap.add_argument("--app")
    ap.add_argument("--data")
    ap.add_argument("--frames", type=int, default=900)
    ap.add_argument("--metal3", action="store_true")
    a = ap.parse_args(argv)
    if a.game:
        g = GAMES[a.game]
        app, data = g["app"], g["data"]
    else:
        app, data = a.app, a.data
    if not app or not data:
        print("give a game, or --app and --data", file=sys.stderr)
        return 2
    d = tempfile.mkdtemp(prefix="nxrt-smoke-")
    shot, log = os.path.join(d, "shot.png"), os.path.join(d, "run.log")
    ok, why = run(app, data, a.frames, not a.metal3, shot, log)
    print(f"{'ok' if ok else 'FAIL'}  {a.game or app}: {why}")
    if not ok:
        print(f"  screenshot: {shot}\n  log: {log}", file=sys.stderr)
        return 1
    print(f"  screenshot: {shot}\n  log: {log}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
