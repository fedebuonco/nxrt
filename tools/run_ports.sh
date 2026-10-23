#!/bin/bash
# Run every port in games/ one after another, capturing each one's log — scripted, or with you playing.
#
#   tools/run_ports.sh                 scripted: 900 frames per game, then it quits by itself
#   tools/run_ports.sh --play          you play each one; close its window and the next starts
#   tools/run_ports.sh --play las sky  just those
#   FRAMES=1500 TIMEOUT=300 tools/run_ports.sh eow
#
# --play leaves the game fully interactive (keyboard, controller, mouse — whatever the port maps): it starts, you play,
# and the harness moves on when the window closes. The log records the whole session either way.
# --shots (the default) runs each game for FRAMES presented frames and exits on its own, for unattended capture.
#
# One game runs at a time: two instances sharing a data root corrupt each other's cache and saves, and they compete for
# the GPU (that made EoW look regressed once already). Output: <repo>/games/logs/run-<date>/<game>.log, <game>.png and
# summary.tsv. The binary is the newest of build-merge/, build-clean/, build-master/, build-wip/, build/ (or BIN_<game>).
# Settings and data root default to this machine's; override with INI_<game> and ROOT_<game>.
set -u

MODE=shots
case ${1:-} in
-h | --help)
    sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
--play | play) MODE=play; shift ;;
--shots | shots) MODE=shots; shift ;;
esac
FRAMES=${FRAMES:-900}          # shots mode: presented frames before the run stops itself
if [ "$MODE" = play ]; then TIMEOUT=${TIMEOUT:-0}; else TIMEOUT=${TIMEOUT:-240}; fi
WINDOW=${WINDOW:-1280x720}
PREP=${NXRT_SHADER_PREP:-}     # NXRT_SHADER_PREP=1: prepare the seen shaders at boot
REPO=$(cd "$(dirname "$0")/.." && pwd)
GAMES=${GAMES_DIR:-$REPO/games}
OUT=${OUT:-$GAMES/logs/run-$(date +%Y%m%d-%H%M%S)}
SUMMARY=$OUT/summary.tsv

# --- per game: the executable's name, settings file, data root ------------------------------------------------------
exe_of() {
    case $1 in
    las) echo lasrecomp ;;
    sky) echo skyrecomp ;;
    eow) echo eowrecomp ;;
    tunic) echo tunicrecomp ;;
    *) echo "${1}recomp" ;;
    esac
}
env_of() {  # env_of INI las -> the value of $INI_LAS
    local k v
    k="$1_$(echo "$2" | tr 'a-z' 'A-Z')"
    eval "v=\${$k:-}"
    echo "$v"
}
ini_of() {
    local v; v=$(env_of INI "$1"); [ -n "$v" ] && { echo "$v"; return; }
    case $1 in
    las) echo /tmp/d/larecomp.ini ;;
    sky) echo /tmp/skydata/skyrecomp.ini ;;
    eow) echo /tmp/eowdata/eowrecomp.ini ;;
    *) echo "" ;;
    esac
}
root_of() {
    local v; v=$(env_of ROOT "$1"); [ -n "$v" ] && { echo "$v"; return; }
    case $1 in
    las) echo /tmp/d ;;
    sky) echo /tmp/skydata ;;
    eow) echo /tmp/eowdata ;;
    *) echo "" ;;
    esac
}
# the newest <game>recomp among the build dirs, unless BIN_<game> names one
bin_of() {
    local v; v=$(env_of BIN "$1"); [ -n "$v" ] && { echo "$v"; return; }
    local exe best="" bt=0 t
    exe=$(exe_of "$1")
    for d in build-merge build-clean build-master build-wip build; do
        [ -x "$GAMES/$1/$d/$exe" ] || continue
        t=$(stat -c %Y "$GAMES/$1/$d/$exe" 2>/dev/null || stat -f %m "$GAMES/$1/$d/$exe" 2>/dev/null || echo 0)
        if [ "$t" -gt "$bt" ]; then bt=$t; best=$GAMES/$1/$d/$exe; fi
    done
    echo "$best"
}

# --- run one game ---------------------------------------------------------------------------------------------------
run_one() {
    local g=$1 bin ini root log png t0 pid wd rc
    bin=$(bin_of "$g"); ini=$(ini_of "$g"); root=$(root_of "$g")
    if [ -z "$bin" ] || [ ! -x "$bin" ]; then
        printf '%-6s SKIP    no binary in games/%s/build*/%s\n' "$g" "$g" "$(exe_of "$g")" | tee -a "$OUT/summary.tsv"
        printf '%s\tSKIP\t-\t-\t-\t-\t-\t-\n' "$g" >> "$SUMMARY"
        return
    fi
    log=$OUT/$g.log; png=$OUT/$g.png
    echo "== $g: $bin  ($MODE)"
    t0=$(date +%s)
    (
        cd "$(dirname "$bin")" || exit 1
        if [ "$MODE" = play ]; then
            NXRT_SETTINGS="$ini" NXRT_DATA_ROOT="$root" NXRT_WINDOW="$WINDOW" \
            ${PREP:+NXRT_SHADER_PREP=$PREP} ./"$(basename "$bin")" --play
        else
            NXRT_SETTINGS="$ini" NXRT_DATA_ROOT="$root" NXRT_WINDOW="$WINDOW" \
            NXRT_WINDOW_SHOT="$FRAMES" NXRT_SHOT_PATH="$png" NXRT_EXIT_AFTER_SHOT=1 \
            ${PREP:+NXRT_SHADER_PREP=$PREP} ./"$(basename "$bin")" --play
        fi
    ) >"$log" 2>&1 &
    pid=$!
    wd=""
    if [ "$TIMEOUT" -gt 0 ] 2>/dev/null; then
        ( sleep "$TIMEOUT"; kill -9 "$pid" 2>/dev/null && echo "[harness] killed after ${TIMEOUT}s" >>"$log" ) &
        wd=$!
    fi
    [ "$MODE" = play ] && echo "   playing — close the window when you are done"
    wait "$pid" 2>/dev/null; rc=$?
    [ -n "$wd" ] && { kill "$wd" 2>/dev/null; wait "$wd" 2>/dev/null; }
    local secs=$(( $(date +%s) - t0 ))
    echo "[harness] $g closed after ${secs}s (exit $rc)" >>"$log"

    # --- what the log says ------------------------------------------------------------------------------------------
    local last fps skipped comp_n comp_ms tfail crash state
    last=$(grep -a '\[metal\] t=' "$log" | tail -1)
    fps=$(printf '%s' "$last" | awk '{for(i=1;i<=NF;i++) if($(i+1)=="fps") print $i}')
    skipped=$(printf '%s' "$last" | sed -n 's/.* ([0-9]* draws, \([0-9]*\) skipped.*/\1/p')
    comp_n=$(grep -ao 'compile [0-9]*/[0-9]*ms' "$log" | tail -1 | sed 's/compile \([0-9]*\)\/.*/\1/')
    comp_ms=$(grep -ao 'compile [0-9]*/[0-9]*ms' "$log" | tail -1 | sed 's/compile [0-9]*\/\([0-9]*\)ms.*/\1/')
    tfail=$(grep -ac 'translate failed' "$log")
    crash=$(grep -aciE 'crashed|SIGSEGV|assertion failed' "$log")
    if grep -aq '\[harness\] killed' "$log"; then state=TIMEOUT
    elif [ "${crash:-0}" -gt 0 ]; then state=CRASH
    elif [ -z "$last" ]; then state=NO-FRAME
    elif [ "${tfail:-0}" -gt 0 ]; then state=WARN
    else state=OK; fi
    printf '%-6s %-7s %6ss  %6s fps  %5s skipped  %4s compiles/%7s ms  %s translate failures\n' \
        "$g" "$state" "$secs" "${fps:--}" "${skipped:--}" "${comp_n:--}" "${comp_ms:--}" "${tfail:-0}" \
        | tee -a "$OUT/summary.txt"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$g" "$state" "$secs" "${fps:--}" "${skipped:--}" "${comp_n:--}" "${comp_ms:--}" "${tfail:-0}" >> "$SUMMARY"
    # (a killed run can leave the render thread holding the GPU; make sure nothing survives into the next game)
    pkill -x "$(basename "$bin")" 2>/dev/null   # (exact name: a -f pattern also matches the shell that typed it)
    sleep 1
}

mkdir -p "$OUT"
printf 'game\tstate\tsecs\tfps\tskipped\tcompiles\tcompile_ms\ttranslate_failures\n' > "$SUMMARY"
if pgrep -f 'recomp' >/dev/null 2>&1; then
    echo "[harness] a game is already running: waiting for it to close"
    while pgrep -f 'recomp' >/dev/null 2>&1; do sleep 1; done
fi

games=("$@")
if [ ${#games[@]} -eq 0 ]; then
    for d in "$GAMES"/*/; do
        g=$(basename "$d")
        [ "$g" = logs ] && continue
        [ -d "$d" ] && games+=("$g")
    done
fi

echo "harness: ${games[*]} -> $OUT ($MODE; timeout ${TIMEOUT}s)"
for g in "${games[@]}"; do
    if [ "$MODE" = play ]; then echo; echo "-- next: $g (starting in 3 s, Ctrl-C to stop)"; sleep 3; fi
    run_one "$g"
done

echo
echo "== summary ($OUT/summary.tsv)"
column -t -s $'\t' "$SUMMARY" 2>/dev/null || cat "$SUMMARY"
