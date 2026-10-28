#!/usr/bin/env python3
"""Smoke-test log checker (tools/smoke.py): crashes and GPU errors are rejected, clean logs pass."""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import smoke  # noqa: E402


def main():
    assert smoke.check_log("[INFO] [metal] saved shot.png\n") == []
    assert len(smoke.check_log("[crash] The game crashed\n")) == 1
    assert len(smoke.check_log("[metal] command buffer error: Caused GPU Address Fault\n")) == 1
    assert len(smoke.check_log("*** signal 11 accessing 0x1\n")) == 1
    assert smoke.check_log("FATAL: no game\n") == ["FATAL: no game"]
    print("test_smoke: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
