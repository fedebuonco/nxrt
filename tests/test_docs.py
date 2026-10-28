#!/usr/bin/env python3
"""Docs link check: every relative Markdown link in the root docs and docs/*.md resolves to a file."""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LINK = re.compile(r"\[[^\]]*\]\(([^)]+)\)")


def main():
    files = [os.path.join(ROOT, f) for f in ("README.md", "STEAM_DECK.md", "THIRD_PARTY.md", "CHANGELOG.md")]
    files += glob.glob(os.path.join(ROOT, "docs", "*.md"))
    bad = []
    checked = 0
    for path in files:
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8") as f:
            text = f.read()
        for m in LINK.finditer(text):
            target = m.group(1).strip()
            if target.startswith(("http://", "https://", "#", "mailto:")):
                continue
            rel = target.split("#", 1)[0]
            if not rel:
                continue
            checked += 1
            dest = os.path.normpath(os.path.join(os.path.dirname(path), rel))
            if not os.path.exists(dest):
                bad.append((os.path.relpath(path, ROOT), target))
    for path, target in bad:
        print(f"{path}: missing {target}", file=sys.stderr)
    if bad:
        print(f"{len(bad)} broken of {checked} links", file=sys.stderr)
        return 1
    print(f"test_docs: {checked} links ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
