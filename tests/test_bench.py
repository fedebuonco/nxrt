#!/usr/bin/env python3
"""Benchmark parser (tools/bench.py): routes, [pace] frames, window selection and percentiles."""
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import bench  # noqa: E402


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)


def main():
    routes = os.path.join(tempfile.mkdtemp(), "bench.tsv")
    with open(routes, "w") as f:
        f.write("# name\tkeys\tstart\tsecs\tabout\n")
        f.write("pan\t1:A,2:B\t45\t55\tthe camera turns\n")
        f.write("stand\t1:A\t10\t5\n")
    r = bench.load_routes(routes)
    check(r["pan"]["start"] == 45 and r["pan"]["secs"] == 55, "route numbers")
    check(r["pan"]["keys"] == "1:A,2:B", "route keys")
    check(r["stand"]["about"] == "", "empty description")

    log = os.path.join(tempfile.mkdtemp(), "run.log")
    with open(log, "w") as f:
        f.write("[init] not a frame\n")
        for i in range(100):
            f.write(f"[pace] period 16.00 blocked 0.50 busy 9.50 cost 10.00 ms (queue wait 0.10, gpu 8.00)\n")
        f.write("[metal] ignored\n")
    frames = bench.parse_log(log)
    check(len(frames) == 100, f"frames {len(frames)}")
    check(frames[0]["period"] == 16.0 and frames[0]["gpu"] == 8.0, "frame fields")

    # 16 ms frames: 62 start times below 1000 ms, the 63rd at 992, the 64th at 1008.
    sel = bench.select(frames, 0, 1.0)
    check(len(sel) == 63, f"window {len(sel)}")
    check(bench.select(frames, 1.0, 0.1)[0]["period"] == 16.0, "offset window")

    check(bench.percentile([1, 2, 3, 4], 50) == 2.5, "p50 interpolation")
    check(bench.percentile([1, 2, 3, 4], 95) == 3.85, "p95 interpolation")
    check(bench.percentile([1, 2, 3, 4], 100) == 4, "p100")
    check(bench.percentile([], 50) is None, "empty percentile")

    s = bench.summarize(sel)
    check(s["frames"] == 63, "summary count")
    check(abs(s["fps"] - 62.5) < 0.01, f"fps {s['fps']}")
    check(s["period_p50"] == 16.0 and s["cost_p50"] == 10.0, "summary percentiles")
    check(s["gpu_p50"] == 8.0, "gpu percentile")
    check(bench.summarize([])["frames"] == 0, "empty summary")

    lab, path = bench.label_path("base=/tmp/x.log")
    check(lab == "base" and path == "/tmp/x.log", "label=path")
    lab, path = bench.label_path("/tmp/base.log")
    check(lab == "base", "basename label")

    # A malformed route is rejected, not silently skipped.
    bad = os.path.join(tempfile.mkdtemp(), "bad.tsv")
    with open(bad, "w") as f:
        f.write("only-a-name\n")
    try:
        bench.load_routes(bad)
    except ValueError:
        pass
    else:
        raise AssertionError("malformed route accepted")

    print("test_bench: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
