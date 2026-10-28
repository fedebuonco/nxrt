#!/bin/sh
# Headless Metal tests (no window): base/specialisation + a red-pixel readback, and the archive round-trip.
# Needs a Metal GPU; run locally, not in the host-only CI job.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
OUT="$ROOT/build/tests"
mkdir -p "$OUT"
CC=${CC:-/usr/bin/clang}
"$CC" -fobjc-arc -I runtime -I runtime/nvn -I runtime/hle -I platform -framework Metal -framework Foundation tests/manual/test_renderer_context.m \
    runtime/nvn/renderer_context.m runtime/nvn/renderer_cache.m -o "$OUT/test_renderer_context"
"$OUT/test_renderer_context"
echo "ok      test_renderer_context"
for t in test_mtl4 test_mtl4_draw test_metal_archive; do
    "$CC" -fobjc-arc -framework Metal -framework Foundation "tests/manual/$t.m" -o "$OUT/$t"
    "$OUT/$t"
    echo "ok      $t"
done
