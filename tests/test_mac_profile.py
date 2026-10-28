#!/usr/bin/env python3
"""Per-game Mac profile schema (tools/mac_profile.py): defaults, values, ranges and typos."""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import mac_profile  # noqa: E402


def fails(text, needle):
    try:
        mac_profile.parse(text)
    except mac_profile.ProfileError as e:
        assert needle in str(e), (needle, str(e))
    else:
        raise AssertionError(f"accepted: {text!r}")


def main():
    d = mac_profile.defaults()
    assert d["scale"] == 1.0 and d["fps"] == 60 and d["upscaler"] == "standard", d
    assert d["tdp_w"] == 0 and d["refresh"] == 0, d

    p = mac_profile.parse("""
# a tested profile
scale\t0.75
fps\t30
frame_cap\t40
upscaler\tmetalfx
fullscreen\t0
refresh\t90
tdp_w\t8
""")
    assert p["scale"] == 0.75 and p["fps"] == 30 and p["frame_cap"] == 40, p
    assert p["upscaler"] == "metalfx" and p["fullscreen"] is False, p
    assert p["refresh"] == 90 and p["tdp_w"] == 8, p

    # `key = value` is accepted too, and comments are skipped.
    p = mac_profile.parse("scale = 0.5\n# note\n")
    assert p["scale"] == 0.5, p

    fails("scale\t1.5", "outside")
    fails("fps\t45", "expected one of")
    fails("upscaler\tbicubic", "expected one of")
    fails("fullscreen\tyes-please", "expected 0/1")
    fails("nonsense\t1", "unknown key")
    fails("scale", "key<TAB>value")
    fails("fps\tmany", "expected one of")

    print("test_mac_profile: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
