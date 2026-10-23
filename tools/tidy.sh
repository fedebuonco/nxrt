#!/bin/sh
# Run clang-tidy with .clang-tidy over the runtime and the layers. Needs a compilation database; this script makes one
# from examples/game-template's Makefile (tools/gen_compile_commands.py) because the project has no build system that
# emits one.
#
#   tools/tidy.sh                     the runtime + layers, C/C++ (a report)
#   tools/tidy.sh --fix               apply the fixes clang-tidy can make (review the diff!)
#   tools/tidy.sh --regen             regenerate build/compile_commands.json first
#   tools/tidy.sh --all               every translation unit (including UI .mm/.m; slower)
#   tools/tidy.sh 'runtime/nvn/.*'    a source-path regex of your own
#
# CLANG_TIDY / RUN_CLANG_TIDY override the tools. The Homebrew LLVM 19 ones are used by default.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT" || exit 2

TIDY=${CLANG_TIDY:-}
RUN=${RUN_CLANG_TIDY:-}
if [ -z "$TIDY" ]; then
    for c in /opt/homebrew/opt/llvm/bin/clang-tidy /usr/local/opt/llvm/bin/clang-tidy clang-tidy; do
        if command -v "$c" >/dev/null 2>&1; then TIDY=$c; break; fi
    done
fi
if [ -z "$TIDY" ]; then
    echo "tidy: clang-tidy not found (install LLVM: brew install llvm)" >&2
    exit 2
fi
if [ -z "$RUN" ]; then
    for c in /opt/homebrew/opt/llvm/bin/run-clang-tidy /usr/local/opt/llvm/bin/run-clang-tidy run-clang-tidy; do
        if command -v "$c" >/dev/null 2>&1; then RUN=$c; break; fi
    done
fi

REGEN=0
FIX=0
ALL=0
SCOPE=
for a in "$@"; do
    case $a in
    --regen) REGEN=1 ;;
    --fix) FIX=1 ;;
    --all) ALL=1 ;;
    -h | --help)
        sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *) SCOPE=$a ;;
    esac
done

# ---- the compile database
if [ "$REGEN" = 1 ] || [ ! -f "$ROOT/build/compile_commands.json" ]; then
    # The generated .inc headers must exist for shader_prep.c / smaa_data.c to parse.
    (cd "$ROOT/examples/game-template" && make -s build/inc/shader_variants.inc build/inc/smaa_hlsl.inc) ||
        { echo "tidy: cannot generate the test build's include files" >&2; exit 2; }
    python3 "$ROOT/tools/gen_compile_commands.py" || exit 2
fi

JOBS=${JOBS:-4}
case "$(uname -s)" in Darwin) JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)} ;; esac
[ "$JOBS" -gt 16 ] 2>/dev/null && JOBS=16

# macOS: give the driver the SDK; Homebrew's clang defaults to a path that may not exist.
EXTRA=
if [ "$(uname -s)" = Darwin ] && command -v xcrun >/dev/null 2>&1; then
    SDK=$(xcrun --show-sdk-path 2>/dev/null || true)
    if [ -n "$SDK" ]; then
        SDKROOT=$SDK
        export SDKROOT
        EXTRA="-extra-arg=-isysroot$SDK"
    fi
fi

if [ "$ALL" = 1 ]; then
    FILTER=
elif [ -n "$SCOPE" ]; then
    FILTER=$SCOPE
else
    FILTER='/(runtime|platform|input|settings|save)/.*\.(c|cpp)$'
fi

LOG="$ROOT/build/tidy.log"
: >"$LOG"
echo "tidy: $TIDY ($($TIDY --version 2>/dev/null | head -1))"
echo "tidy: report -> $LOG"

STATUS=0
if [ -n "$RUN" ]; then
    if [ "$FIX" = 1 ]; then
        # shellcheck disable=SC2086
        "$RUN" -p "$ROOT/build" -j "$JOBS" -clang-tidy-binary "$TIDY" \
            -clang-apply-replacements-binary "$(dirname "$TIDY")/clang-apply-replacements" \
            -quiet $EXTRA -fix $FILTER >"$LOG" 2>&1
        STATUS=$?
    else
        # shellcheck disable=SC2086
        "$RUN" -p "$ROOT/build" -j "$JOBS" -clang-tidy-binary "$TIDY" -quiet $EXTRA $FILTER >"$LOG" 2>&1
        STATUS=$?
    fi
else
    # No run-clang-tidy: one process per file from the database.
    files=$(python3 - "$ROOT/build/compile_commands.json" "$FILTER" <<'PY'
import json, re, sys
rx = sys.argv[2]
for e in json.load(open(sys.argv[1])):
    if not rx or re.search(rx, e["file"]):
        print(e["file"])
PY
)
    : >"$LOG"
    for f in $files; do
        # shellcheck disable=SC2086
        "$TIDY" -p "$ROOT/build" $EXTRA "$f" >>"$LOG" 2>&1
    done
fi

# clang-analyzer diagnostics that point into a vendored header are attributed to the including main file, so the
# header filters above do not drop them: remove any diagnostic block that starts in third_party/.
awk '/: (warning|error):/ { skip = ($0 ~ /third_party\//) } !skip' "$LOG" >"$LOG.tmp" && mv "$LOG.tmp" "$LOG"

warnings=$(grep -c 'warning:' "$LOG" 2>/dev/null || true)
errors=$(grep -c 'error:' "$LOG" 2>/dev/null || true)
echo "tidy: third_party diagnostics excluded"
echo "tidy: $warnings warning(s), $errors error(s) (full report: $LOG)"
[ "$STATUS" -eq 0 ] || exit "$STATUS"
