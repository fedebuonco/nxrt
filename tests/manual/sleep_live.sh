#!/bin/bash
# A real macOS sleep/wake test for A9. Run from a terminal, play/load a save, sleep the Mac, wake it, quit.
set -u
APP=${APP:-/Volumes/FE3HDev/fe3h/build-macoscore/fe3hrecomp}
DATA=${DATA:-/Volumes/FE3HDev/fe3h}
LOG=${LOG:-/tmp/nxrt-sleep-live.log}
[ -x "$APP" ] || { echo "no app: $APP (build the FE3H port with B=build-macoscore first)"; exit 2; }
: >"$LOG"
echo "Launching $APP (data $DATA). Log: $LOG"
NXRT_DATA_ROOT="$DATA" NXRT_NO_LAUNCHER=1 NXRT_METAL4=0 "$APP" >>"$LOG" 2>&1 &
PID=$!
cat <<'MSG'

1. Let the game reach gameplay and load a save (or start one).
2. Put the Mac to sleep (Apple menu > Sleep), wait a few seconds, wake it.
3. Check that audio resumes and the picture is intact; quit the game (Cmd-Q).
4. Press Return here.

MSG
read -r _
if kill -0 "$PID" 2>/dev/null; then
    echo "game still running: quit it (Cmd-Q), then press Return"
    read -r _
fi
wait "$PID" 2>/dev/null
echo "--- sleep/wake lines ---"
grep -n "\[sleep\]" "$LOG" || echo "(none: no NSWorkspace notification reached the runtime)"
echo "--- crashes / errors ---"
grep -n "\[crash\]\|signal 11\|FATAL" "$LOG" || echo "(none)"
echo "--- audio ---"
grep -n "audio" "$LOG" | tail -5 || true
echo "Log: $LOG"
