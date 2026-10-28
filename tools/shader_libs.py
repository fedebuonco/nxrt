#!/usr/bin/env python3
"""Compile translated MSL to Metal libraries at setup, so play never runs the MSL compiler.

nxnative compiles its shaders at pack time and ships the libraries. nxrt translates the game's shaders on the player's
Mac; this tool is the setup step that turns each translated source into a `.metallib` (Metal's compiled library), which
the runtime loads with `newLibraryWithURL` instead of `newLibraryWithSource`.

    python3 tools/shader_libs.py shaders/ out/            # every *.metal in shaders/
    python3 tools/shader_libs.py one.metal out/one.metallib

The runtime looks for `cache/shaders/<hash>.metallib`; the setup tool is expected to write the same names (the hash is
the runtime's `shader_source_key`, documented in runtime/nvn/renderer.m). Until it does, the runtime falls back to
compiling the source, exactly as before.
"""
import argparse
import os
import subprocess
import sys
import tempfile

FNV_OFFSET = 0xcbf29ce484222325
FNV_PRIME = 0x100000001b3


def source_key(text):
    """FNV-1a 64 of the exact MSL bytes: the runtime's cache name (renderer.m shader_source_key)."""
    h = FNV_OFFSET
    for b in text.encode("utf-8"):
        h ^= b
        h = (h * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def run(cmd):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if p.returncode:
        raise RuntimeError(f"{' '.join(cmd)} failed:\n{p.stderr.strip()}")
    return p


def compile_one(src, out, sdk="macosx"):
    """One .metal -> .metallib (air + metallib in a temp dir; the output is written only on success)."""
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="nxrt-metal-") as d:
        air = os.path.join(d, "shader.air")
        run(["xcrun", "-sdk", sdk, "metal", "-c", src, "-o", air])
        tmp = out + ".tmp"
        run(["xcrun", "-sdk", sdk, "metallib", air, "-o", tmp])
        os.replace(tmp, out)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="a .metal file, or a directory of them")
    ap.add_argument("output", help="a .metallib path, or a directory for a directory of sources")
    ap.add_argument("--sdk", default="macosx")
    ap.add_argument("--cache-dir", help="write <runtime source hash>.metallib here (the runtime's cache/shaders)")
    a = ap.parse_args(argv)
    try:
        if os.path.isdir(a.source):
            os.makedirs(a.output, exist_ok=True)
            n = 0
            for name in sorted(os.listdir(a.source)):
                if name.startswith("._") or not name.endswith(".metal"):  # (AppleDouble files on external drives)
                    continue
                src = os.path.join(a.source, name)
                out = os.path.join(a.output, os.path.splitext(name)[0] + ".metallib")
                if a.cache_dir:
                    out = os.path.join(a.cache_dir, f"{source_key(open(src, encoding='utf-8').read()):016x}.metallib")
                compile_one(src, out, a.sdk)
                n += 1
            print(f"{n} libraries -> {a.output}")
            return 0
        out = a.output
        if a.cache_dir:
            out = os.path.join(a.cache_dir, f"{source_key(open(a.source, encoding='utf-8').read()):016x}.metallib")
        compile_one(a.source, out, a.sdk)
        print(out)
        return 0
    except (OSError, RuntimeError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
