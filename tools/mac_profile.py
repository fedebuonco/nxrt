#!/usr/bin/env python3
"""A per-game Mac profile: the tested defaults a port ships, in one small file.

Format: `key<TAB>value`, comments on their own line. Unknown keys are errors (a typo must not silently do nothing).
The runtime/UI integration is a separate step; this module is the schema and validator the packaging tools use.

    scale       0.5 .. 1.0     render scale (1.0 keeps the game's own)
    fps         30 | 60        the game's frame-rate target
    frame_cap   0 | 30 | 40 | 60 | 120    a cap; 0 follows the game
    upscaler    standard | metalfx
    fullscreen  1 | 0
    refresh     0 | 60 | 90 | 120        0: follow the display
    tdp_w       0 .. 15        a recommendation only; an app generally cannot set TDP
"""
import argparse
import sys

SCHEMA = {
    "scale": ("float", 1.0, 0.5, 1.0),
    "fps": ("choice", 60, (30, 60), None),
    "frame_cap": ("choice", 0, (0, 30, 40, 60, 120), None),
    "upscaler": ("choice", "standard", ("standard", "metalfx"), None),
    "fullscreen": ("bool", True, None, None),
    "refresh": ("choice", 0, (0, 60, 90, 120), None),
    "tdp_w": ("int", 0, 0, 15),
}


class ProfileError(ValueError):
    pass


def defaults():
    return {k: v[1] for k, v in SCHEMA.items()}


def _value(key, raw):
    if key not in SCHEMA:
        raise ProfileError(f"unknown key {key!r}")
    kind, default, lo, hi = SCHEMA[key]
    if kind == "bool":
        s = raw.strip().lower()
        if s in ("1", "true", "on", "yes"):
            return True
        if s in ("0", "false", "off", "no"):
            return False
        raise ProfileError(f"{key}: expected 0/1, got {raw!r}")
    if kind == "choice":
        try:
            v = int(raw, 0) if isinstance(default, int) else raw.strip().lower()
        except ValueError:
            raise ProfileError(f"{key}: expected one of {lo}, got {raw!r}")
        if v not in lo:
            raise ProfileError(f"{key}: expected one of {lo}, got {raw!r}")
        return v
    try:
        v = float(raw) if kind == "float" else int(raw, 0)
    except ValueError:
        raise ProfileError(f"{key}: expected a number, got {raw!r}")
    if v < lo or v > hi:
        raise ProfileError(f"{key}: {v} is outside [{lo}, {hi}]")
    return v


def parse(text):
    p = defaults()
    for n, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "\t" in line:
            key, raw = line.split("\t", 1)
        elif "=" in line:
            key, raw = line.split("=", 1)
        else:
            raise ProfileError(f"line {n}: expected key<TAB>value")
        key = key.strip()
        if not key:
            raise ProfileError(f"line {n}: empty key")
        p[key] = _value(key, raw)
    return p


def load(path):
    with open(path, encoding="utf-8") as f:
        return parse(f.read())


def main(argv=None):
    ap = argparse.ArgumentParser(description="Validate a per-game Mac profile (mac.tsv).")
    ap.add_argument("path")
    ap.add_argument("--json", action="store_true", help="print the resolved profile as JSON")
    a = ap.parse_args(argv)
    try:
        p = load(a.path)
    except (OSError, ProfileError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    if a.json:
        import json
        print(json.dumps(p, indent=2, sort_keys=True))
    else:
        for k in SCHEMA:
            print(f"{k}\t{p[k]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
