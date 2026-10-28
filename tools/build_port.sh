#!/bin/sh
# One-command port build against this checkout.
#   tools/build_port.sh fe3h
#   tools/build_port.sh eow
#   SMOKE=1 tools/build_port.sh eow
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
port=${1:?usage: build_port.sh fe3h|eow}
case "$port" in
fe3h) dir=${FE3H_DIR:-/Volumes/FE3HDev/fe3h} ;;
eow)  dir=${EOW_DIR:-/Volumes/PortableSSD/eow-macoscore} ;;
*) echo "unknown port: $port" >&2; exit 2 ;;
esac
app=$(grep -E '^GAME_EXE' "$dir/Makefile" | head -1 | awk -F:= '{print $2}' | tr -d ' ')
b=${B:-build-macoscore}
make -C "$dir" -j8 B="$b" NXRT="$ROOT" CLANG=/usr/bin/clang
echo "built $dir/$b/$app"
if [ "${SMOKE:-0}" = 1 ]; then
    python3 "$ROOT/tools/smoke.py" "$port"
fi
