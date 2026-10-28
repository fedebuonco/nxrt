#!/usr/bin/env python3
"""Survey a game module (NSO): build id, segment sizes, imports/exports, and how many imports the runtime's HLE covers.

This is the first half of C5 (survey + function matching): before a new title boots, show what it needs. Function
matching across builds (nxnative's nso_fnmap/MATCH_CUTS) is the other half and needs the real NSOs.

    python3 tools/survey.py <main NSO> [--hle runtime/hle/hle_symbols.txt] [--json]
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nso import Nso  # noqa: E402


def load_hle(path):
    if not path or not os.path.exists(path):
        return set()
    return {line.strip() for line in open(path, encoding="utf-8") if line.strip() and not line.startswith("#")}


def summarize(nso, hle=()):
    imports = [s[0] for s in nso.imports()]
    exports = [s[0] for s in nso.exports()]
    hle = set(hle)
    covered = sorted(hle & set(imports))
    missing = sorted(set(imports) - hle) if hle else []
    return {
        "build_id": nso.build_id,
        "text": nso.text[1],
        "rodata": nso.rodata[1],
        "data": nso.data[1],
        "bss": getattr(nso, "bss_size", 0),
        "image": getattr(nso, "size", 0),
        "imports": len(imports),
        "exports": len(exports),
        "hle_covered": len(covered),
        "hle_missing": len(missing),
        "missing": missing,
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("nso")
    ap.add_argument("--hle", default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                                  "runtime", "hle", "hle_symbols.txt"))
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    try:
        s = summarize(Nso(a.nso), load_hle(a.hle))
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    if a.json:
        print(json.dumps(s, indent=2, sort_keys=True))
    else:
        print(f"build id   {s['build_id']}")
        print(f"text/ro/data/bss  {s['text']}/{s['rodata']}/{s['data']}/{s['bss']} bytes")
        print(f"imports    {s['imports']} ({s['hle_covered']} HLE covered, {s['hle_missing']} missing)")
        print(f"exports    {s['exports']}")
        for m in s["missing"][:40]:
            print(f"  missing  {m}")
        if len(s["missing"]) > 40:
            print(f"  ... {len(s['missing']) - 40} more")
    return 0


if __name__ == "__main__":
    sys.exit(main())
