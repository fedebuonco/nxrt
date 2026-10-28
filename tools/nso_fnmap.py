#!/usr/bin/env python3
"""Map functions between two builds of one module by exact fingerprint (tools/fnmap.py).

    python3 tools/nso_fnmap.py old/main.nso new/main.nso [--json]

Addresses are module offsets (the caller adds the module base). Unmatched functions are printed with `-` so a patch
carried across a build can be reviewed, not silently misapplied.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from analyze import eh_frame_functions  # noqa: E402
from fnmap import fingerprint, match_functions  # noqa: E402
from nso import Nso  # noqa: E402


def functions(path):
    n = Nso(path)
    starts = sorted(set(eh_frame_functions(n)))
    out = []
    for i, s in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else len(n.image)
        if end - s >= 4:
            out.append((s, bytes(n.image[s:end])))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("old")
    ap.add_argument("new")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    try:
        fa, fb = functions(a.old), functions(a.new)
        m = match_functions(fa, fb)
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    if a.json:
        print(json.dumps({"old": len(fa), "new": len(fb), "matched": len(m),
                          "map": {f"{k:#x}": f"{v:#x}" for k, v in sorted(m.items())}}, indent=2))
    else:
        for addr, data in fa:
            print(f"{addr:#x}\t{m.get(addr, '-') if addr in m else '-'}\t{fingerprint(data):016x}")
    print(f"# {len(m)} matched of {len(fa)} old / {len(fb)} new", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
