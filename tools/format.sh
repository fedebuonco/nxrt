#!/bin/sh
# Format the C/C++/ObjC sources with clang-format and .clang-format (LLVM's style, 4 spaces, 120 columns).
#
#   tools/format.sh                 check every tracked source (exit 1 if any needs formatting)
#   tools/format.sh --fix           rewrite them
#   tools/format.sh --fix runtime/loader.c tests/
#
# CLANG_FORMAT=/path/to/clang-format overrides the tool. The one from Homebrew LLVM is used by default:
# Apple's clang doesn't ship clang-format.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT" || exit 2

CF=${CLANG_FORMAT:-}
if [ -z "$CF" ]; then
    for c in /opt/homebrew/opt/llvm/bin/clang-format /usr/local/opt/llvm/bin/clang-format clang-format; do
        if command -v "$c" >/dev/null 2>&1; then CF=$c; break; fi
    done
fi
if [ -z "$CF" ]; then
    echo "format: clang-format not found (install LLVM: brew install llvm)" >&2
    exit 2
fi

MODE=check
case ${1:-} in
--fix | -i | fix) MODE=fix; shift ;;
--check | -n | check) MODE=check; shift ;;
-h | --help)
    sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
esac

if [ $# -gt 0 ]; then
    FILES=$(find "$@" -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.m' -o -name '*.mm' \) 2>/dev/null)
else
    # Tracked files plus the new, not-yet-committed ones; vendored code and build output stay out.
    FILES=$(git ls-files --cached --others --exclude-standard -- '*.c' '*.h' '*.cpp' '*.m' '*.mm' |
        grep -vE '^(third_party|build|games)/' || true)
fi
if [ -z "$FILES" ]; then
    echo "format: no C/C++/ObjC files"
    exit 0
fi

bad=0
n=0
for f in $FILES; do
    n=$((n + 1))
    if [ "$MODE" = fix ]; then
        if ! "$CF" -i "$f"; then
            echo "format: cannot format $f" >&2
            exit 2
        fi
    else
        if ! "$CF" --dry-run --Werror "$f" >/dev/null 2>&1; then
            echo "needs formatting: $f"
            bad=$((bad + 1))
        fi
    fi
done

if [ "$MODE" = fix ]; then
    echo "format: rewrote $n files with $CF"
    exit 0
fi
if [ "$bad" -gt 0 ]; then
    echo "format: $bad of $n files need formatting (run tools/format.sh --fix)" >&2
    exit 1
fi
echo "format: $n files are formatted"
