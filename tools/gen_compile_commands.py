#!/usr/bin/env python3
"""Write build/compile_commands.json from the game template's `make -n -B runtime-objs` output.

The runtime has no generated compilation database: it is a set of Makefile includes. `make -n -B` prints every
compile command without running it (the generated .inc files must already exist: run `make runtime-objs` once from
examples/game-template), which is enough for clang-tidy/clangd to see the real include paths and defines.

Usage: tools/gen_compile_commands.py [--template DIR] [--out FILE]
"""
import argparse
import json
import os
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXTS = (".c", ".cc", ".cpp", ".cxx", ".m", ".mm")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--template", default=os.path.join(ROOT, "examples", "game-template"),
                    help="the Makefile to ask (default: examples/game-template)")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "compile_commands.json"))
    a = ap.parse_args()

    try:
        out = subprocess.run(["make", "-n", "-B", "runtime-objs"], cwd=a.template, check=True,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"gen_compile_commands: make failed: {e}", file=sys.stderr)
        if isinstance(e, subprocess.CalledProcessError) and e.stderr:
            print(e.stderr, file=sys.stderr)
        return 1

    entries = []
    for line in out.splitlines():
        line = line.strip()
        if " -c " not in line:
            continue
        try:
            args = shlex.split(line)
        except ValueError:
            continue
        if "-c" not in args:
            continue
        src = args[args.index("-c") + 1]
        if not src.endswith(EXTS):
            continue
        # Drop -o <obj>: clang-tidy only needs the frontend flags, and the object path may not exist yet.
        cleaned = []
        i = 0
        while i < len(args):
            if args[i] == "-o":
                i += 2
                continue
            cleaned.append(args[i])
            i += 1
        entries.append({
            "directory": os.path.abspath(a.template),
            "file": os.path.normpath(os.path.join(a.template, src)),
            "command": " ".join(shlex.quote(x) for x in cleaned),
        })

    if not entries:
        print("gen_compile_commands: no compile commands (run `make runtime-objs` in examples/game-template first)",
              file=sys.stderr)
        return 1
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w") as f:
        json.dump(entries, f, indent=1)
        f.write("\n")
    print(f"gen_compile_commands: {len(entries)} entries -> {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
