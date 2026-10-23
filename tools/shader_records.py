#!/usr/bin/env python3
"""Merge, validate and sort nxrt shader variant records (the lines NXRT_RECORD_KEYS writes).

  tools/shader_records.py check FILE...
  tools/shader_records.py merge OUT FILE...
  tools/shader_records.py stats FILE...

A record is one line: `G vs=<hash> fs=<hash> ...` or `C cs=<hash> l=x,y,z ...`. This tool does not know the
pipeline fields' meaning; it checks the envelope, deduplicates exact lines and keeps a stable order so a shipped
list diffs cleanly. The runtime's shader_rec_parse remains the authority on whether the fields are usable.
"""
import argparse
import re
import sys

HASH = re.compile(r"^(?:vs|fs|cs)=([0-9a-fA-F]{1,16})$")


def fields(line):
    out = {}
    for part in line.split():
        if "=" in part:
            k, v = part.split("=", 1)
            out[k] = v
    return out


def check_line(line, where):
    """Returns an error string, or None. The full state is validated by shader_rec_parse at build time."""
    if not line or line[0] not in "GC":
        return "not a G/C record"
    if line[0] == "C":
        f = fields(line)
        if "cs" not in f:
            return "compute record without cs="
        if "l" not in f or not re.fullmatch(r"\d+,\d+,\d+", f.get("l", "")):
            return "compute record without l=x,y,z"
        for k in ("cs",):
            if not HASH.match(f"{k}={f[k]}"):
                return f"bad {k} hash"
        return None
    f = fields(line)
    for k in ("vs", "fs"):
        if k not in f:
            return f"graphics record without {k}="
        if not HASH.match(f"{k}={f[k]}"):
            return f"bad {k} hash"
    for k in ("a", "s", "c", "d", "n", "m", "b", "B", "k", "t", "p", "f"):
        if k not in f:
            return f"graphics record without {k}="
    return None


def read(paths, strict):
    seen = set()
    lines = []
    bad = 0
    for path in paths:
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                for n, raw in enumerate(f, 1):
                    line = raw.strip()
                    if not line or line.startswith("#"):
                        continue
                    err = check_line(line, f"{path}:{n}")
                    if err:
                        bad += 1
                        if strict:
                            print(f"{path}:{n}: {err}: {line[:120]}", file=sys.stderr)
                        continue
                    if line not in seen:
                        seen.add(line)
                        lines.append(line)
        except OSError as e:
            print(f"{path}: {e}", file=sys.stderr)
            if strict:
                raise
    return lines, bad


def key(line):
    f = fields(line)
    return (0 if line[0] == "G" else 1, int(f.get("vs", "0"), 16), int(f.get("fs", "0"), 16), line)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("check")
    p.add_argument("files", nargs="+")
    p = sub.add_parser("merge")
    p.add_argument("out")
    p.add_argument("files", nargs="+")
    p = sub.add_parser("stats")
    p.add_argument("files", nargs="+")
    a = ap.parse_args()

    if a.cmd == "check":
        lines, bad = read(a.files, strict=True)
        print(f"{len(lines)} records, {bad} invalid")
        return 1 if bad else 0

    if a.cmd == "merge":
        lines, bad = read(a.files, strict=False)
        lines.sort(key=key)
        with open(a.out, "w", encoding="utf-8") as f:
            for line in lines:
                f.write(line + "\n")
        print(f"{a.out}: {len(lines)} unique records ({bad} invalid skipped)")
        return 1 if bad else 0

    lines, bad = read(a.files, strict=False)
    g = sum(1 for l in lines if l[0] == "G")
    c = sum(1 for l in lines if l[0] == "C")
    vs = {fields(l).get("vs") for l in lines if "vs" in fields(l)}
    fs = {fields(l).get("fs") for l in lines if "fs" in fields(l)}
    cs = {fields(l).get("cs") for l in lines if "cs" in fields(l)}
    print(f"{len(lines)} records: {g} graphics, {c} compute | {len(vs)} vs, {len(fs)} fs, {len(cs)} cs | {bad} invalid")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
