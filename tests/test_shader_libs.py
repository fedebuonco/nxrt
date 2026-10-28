#!/usr/bin/env python3
"""Setup-time MSL compiler (tools/shader_libs.py): a real .metallib and the runtime cache name."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import shader_libs  # noqa: E402

MSL = """#include <metal_stdlib>
using namespace metal;
vertex float4 main0(uint vid [[vertex_id]]) { return float4(1.0); }
"""


def main():
    # Shared vectors with runtime/nvn/shader_source_key.c (tests/test_shader_key.c).
    assert shader_libs.source_key("abc") == 0xE71FA2190541574B
    assert shader_libs.source_key("hello") == 0xA430D84680AABD0B

    d = tempfile.mkdtemp()
    src = os.path.join(d, "s.metal")
    open(src, "w").write(MSL)
    out = os.path.join(d, "s.metallib")
    p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "shader_libs.py"), src, out],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert p.returncode == 0, p.stderr
    assert os.path.getsize(out) > 0

    cache = os.path.join(d, "cache")
    p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "shader_libs.py"), src, os.path.join(d, "x.metallib"),
                        "--cache-dir", cache], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert p.returncode == 0, p.stderr
    name = f"{shader_libs.source_key(MSL):016x}.metallib"
    assert os.path.isfile(os.path.join(cache, name)), (name, os.listdir(cache))

    # Directory mode: each source, its own runtime cache name.
    srcdir, outdir, cachedir = os.path.join(d, "src"), os.path.join(d, "out"), os.path.join(d, "cache2")
    os.makedirs(srcdir)
    open(os.path.join(srcdir, "a.metal"), "w").write(MSL)
    open(os.path.join(srcdir, "b.metal"), "w").write(MSL.replace("1.0", "0.5"))
    open(os.path.join(srcdir, "._b.metal"), "wb").write(b"\x00\xb0AppleDouble")  # (external-drive metadata)
    p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "shader_libs.py"), srcdir, outdir,
                        "--cache-dir", cachedir], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert p.returncode == 0, p.stderr
    assert len([f for f in os.listdir(cachedir) if f.endswith(".metallib")]) == 2, os.listdir(cachedir)

    bad = os.path.join(d, "bad.metal")
    open(bad, "w").write("this is not metal\n")
    p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "shader_libs.py"), bad, os.path.join(d, "bad.metallib")],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert p.returncode == 2, p.returncode

    print("test_shader_libs: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
