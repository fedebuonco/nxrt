#!/usr/bin/env python3
"""Module survey (tools/survey.py): sizes, imports/exports and HLE coverage."""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import survey  # noqa: E402


class FakeNso:
    build_id = "00112233445566778899aabbccddeeff"
    text = (0, 0x1000)
    rodata = (0x1000, 0x200)
    data = (0x1200, 0x100)
    bss_size = 0x80
    size = 0x2000

    def imports(self):
        return [("nn::os::Foo", 0, 0, 0, 0), ("libnx", 0, 0, 0, 0), ("nn::fs::Bar", 0, 0, 0, 0)]

    def exports(self):
        return [("main", 0, 1, 0, 0)]


def main():
    s = survey.summarize(FakeNso(), {"nn::os::Foo", "unused"})
    assert s["build_id"] == FakeNso.build_id
    assert s["text"] == 0x1000 and s["bss"] == 0x80
    assert s["imports"] == 3 and s["exports"] == 1
    assert s["hle_covered"] == 1 and s["hle_missing"] == 2
    assert s["missing"] == ["libnx", "nn::fs::Bar"], s["missing"]

    # No HLE set: nothing is claimed covered or missing.
    s = survey.summarize(FakeNso(), set())
    assert s["hle_covered"] == 0 and s["hle_missing"] == 0 and s["missing"] == []

    print("test_survey: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
