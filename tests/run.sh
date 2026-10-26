#!/bin/sh
# Host tests for the parts of nxrt that don't need a running game: the settings/save/mods layers, the recompiler's
# NSO loader, the runtime's parsers and layout code, and the shared crypto/hash helpers. Each test compiles the code
# under test for real (no mocks of the functions themselves) with ASan+UBSan by default.
#
#   tests/run.sh            all tests
#   tests/run.sh lz4 romfs  only the tests whose names contain one of the arguments
#   SAN=0 tests/run.sh      no sanitizers (faster, still runs every check)
#   CC=... CXX=...          another toolchain
#
# SDL3 is used by test_bindings (skipped when it isn't installed); liblz4 is used as the reference in test_lz4 (the
# test still runs without it). Python tests use the tools/venv's packages if present, else the system's.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT" || exit 2
OUT="$ROOT/build/tests"
mkdir -p "$OUT"

# ---- toolchain
case "$(uname -s)" in
Darwin)
    if [ -z "${SDKROOT:-}" ]; then SDKROOT=$(xcrun --show-sdk-path 2>/dev/null || true); export SDKROOT; fi
    [ -z "${CC:-}" ] && [ -x /usr/bin/clang ] && CC=/usr/bin/clang
    [ -z "${CXX:-}" ] && [ -x /usr/bin/clang++ ] && CXX=/usr/bin/clang++
    ;;
esac
CC=${CC:-cc}
CXX=${CXX:-c++}

SAN=${SAN-address,undefined}
[ "$SAN" = 0 ] && SAN=
SANFLAGS=
[ -n "$SAN" ] && SANFLAGS="-fsanitize=$SAN"
if [ -z "${ASAN_OPTIONS:-}" ]; then
    case "$(uname -s)" in
    Darwin) ASAN_OPTIONS=detect_leaks=0 ;;  # (not supported by Apple's ASan runtime)
    *) ASAN_OPTIONS=detect_leaks=1 ;;
    esac
fi
export ASAN_OPTIONS
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"

CFLAGS="-std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -fno-omit-frame-pointer $SANFLAGS"
CXXFLAGS="-std=c++17 -O1 -g -Wall -Wextra -fno-omit-frame-pointer $SANFLAGS"
INC="-I$ROOT/tests -I$ROOT/manage -I$ROOT/runtime -I$ROOT/runtime/nvn -I$ROOT/runtime/hle -I$ROOT/platform -I$ROOT/settings \
     -I$ROOT/save -I$ROOT/input"

# ---- optional dependencies
HAVE_LZ4=0
LZ4_CFLAGS=
LZ4_LIBS=
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists liblz4 2>/dev/null; then
    LZ4_CFLAGS=$(pkg-config --cflags liblz4)
    LZ4_LIBS=$(pkg-config --libs liblz4)
    HAVE_LZ4=1
elif [ -f /opt/homebrew/opt/lz4/include/lz4.h ]; then
    LZ4_CFLAGS="-I/opt/homebrew/opt/lz4/include"
    LZ4_LIBS="-L/opt/homebrew/opt/lz4/lib -llz4 -Wl,-rpath,/opt/homebrew/opt/lz4/lib"
    HAVE_LZ4=1
elif [ -f /usr/local/opt/lz4/include/lz4.h ]; then
    LZ4_CFLAGS="-I/usr/local/opt/lz4/include"
    LZ4_LIBS="-L/usr/local/opt/lz4/lib -llz4 -Wl,-rpath,/usr/local/opt/lz4/lib"
    HAVE_LZ4=1
elif [ -f /usr/include/lz4.h ]; then
    LZ4_LIBS="-llz4"
    HAVE_LZ4=1
fi
LZ4_DEF=
[ "$HAVE_LZ4" = 1 ] && LZ4_DEF="-DHAVE_LZ4=1"

HAVE_SDL=0
SDL_CFLAGS=
SDL_LIBS=
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists sdl3 2>/dev/null; then
    SDL_CFLAGS=$(pkg-config --cflags sdl3)
    SDL_LIBS=$(pkg-config --libs sdl3)
    HAVE_SDL=1
elif [ -f /opt/homebrew/opt/sdl3/include/SDL3/SDL.h ]; then
    SDL_CFLAGS="-I/opt/homebrew/opt/sdl3/include"
    SDL_LIBS="-L/opt/homebrew/opt/sdl3/lib -lSDL3 -Wl,-rpath,/opt/homebrew/opt/sdl3/lib"
    HAVE_SDL=1
elif [ -f /usr/local/opt/sdl3/include/SDL3/SDL.h ]; then
    SDL_CFLAGS="-I/usr/local/opt/sdl3/include"
    SDL_LIBS="-L/usr/local/opt/sdl3/lib -lSDL3 -Wl,-rpath,/usr/local/opt/sdl3/lib"
    HAVE_SDL=1
fi

PY=
if [ -x "$ROOT/tools/venv/bin/python" ]; then PY="$ROOT/tools/venv/bin/python"
elif command -v python3 >/dev/null 2>&1; then PY=python3
fi
HAVE_PY_LZ4=0
[ -n "$PY" ] && "$PY" -c 'import lz4.block' >/dev/null 2>&1 && HAVE_PY_LZ4=1
HAVE_PY_RECOMP=0
[ -n "$PY" ] && "$PY" -c 'import lz4.block, capstone, unicorn' >/dev/null 2>&1 && HAVE_PY_RECOMP=1

PASS=0
FAIL=0
SKIP=0
FILTER="$*"

wanted() {  # wanted <name>: a filter argument means a substring match
    [ -z "$FILTER" ] && return 0
    for f in $FILTER; do case "$1" in *"$f"*) return 0 ;; esac; done
    return 1
}

report() {  # report <status> <name> <log>
    case "$1" in
    ok) PASS=$((PASS + 1)); printf 'ok      %-16s %s\n' "$2" "$(tail -n 1 "$3" 2>/dev/null)" ;;
    fail) FAIL=$((FAIL + 1)); printf 'FAIL    %-16s (see %s)\n' "$2" "$3"; tail -n 25 "$3" ;;
    skip) SKIP=$((SKIP + 1)); printf 'skip    %-16s %s\n' "$2" "$(cat "$3" 2>/dev/null)" ;;
    esac
}

run_c() {  # run_c <name> <source...> [-- <link flags...>]
    name=$1; shift
    wanted "$name" || return 0
    log="$OUT/$name.log"
    srcs=
    libs=
    seen_libs=0
    for a in "$@"; do
        if [ "$a" = "--" ]; then seen_libs=1; continue; fi
        if [ "$seen_libs" = 1 ]; then libs="$libs $a"; else srcs="$srcs $a"; fi
    done
    # shellcheck disable=SC2086
    if ! $CC $CFLAGS $INC $LZ4_DEF $srcs $libs -o "$OUT/$name" >"$log" 2>&1; then
        report fail "$name" "$log"
        return 0
    fi
    if "$OUT/$name" >>"$log" 2>&1; then report ok "$name" "$log"; else report fail "$name" "$log"; fi
}

run_cxx() {
    name=$1; shift
    wanted "$name" || return 0
    log="$OUT/$name.log"
    if ! $CXX $CXXFLAGS $INC "$@" -o "$OUT/$name" >"$log" 2>&1; then
        report fail "$name" "$log"
        return 0
    fi
    if "$OUT/$name" >>"$log" 2>&1; then report ok "$name" "$log"; else report fail "$name" "$log"; fi
}

run_py() {
    name=$1; shift
    wanted "$name" || return 0
    log="$OUT/$name.log"
    if [ -z "$PY" ]; then echo "no python3" >"$log"; report skip "$name" "$log"; return 0; fi
    if ! "$PY" "$@" >"$log" 2>&1; then report fail "$name" "$log"; return 0; fi
    report ok "$name" "$log"
}

echo "nxrt host tests ($CC, SAN=${SAN:-off})"

# C runtime units
run_c test_lz4 tests/test_lz4.c runtime/lz4_block.c runtime/nxpkg.c -- -pthread $LZ4_CFLAGS $LZ4_LIBS
run_c test_aes tests/test_aes.c runtime/nxpkg.c runtime/lz4_block.c -- -pthread
run_c test_nxpkg tests/test_nxpkg.c runtime/nxpkg.c runtime/lz4_block.c -- -pthread
run_c test_tegra tests/test_tegra.c runtime/nvn/tegra.c runtime/nvn/formats.c
run_c test_romfs tests/test_romfs.c runtime/romfs_build.c
run_c test_manage tests/test_manage.c manage/nxrt_manage.c runtime/nxpkg.c runtime/lz4_block.c runtime/romfs_build.c -- -pthread
run_c test_hle_libc tests/test_hle_libc.c runtime/hle/libc.c
run_c test_gamelib_id tests/test_gamelib_id.c

# layers
run_c test_log tests/test_log.c platform/nx_log.c -- -pthread
run_c test_settings tests/test_settings.c settings/settings.c platform/nx_log.c -- -pthread
run_c test_mods tests/test_mods.c runtime/mods.c platform/nx_log.c -- -pthread
run_c test_save tests/test_save.c save/save.c platform/nx_log.c -- -pthread
run_cxx test_sha256 tests/test_sha256.cpp

# SDL: scancode names
if [ "$HAVE_SDL" = 1 ]; then
    wanted test_bindings && {
        name=test_bindings
        log="$OUT/$name.log"
        # shellcheck disable=SC2086
        if $CC $CFLAGS $INC $SDL_CFLAGS tests/test_bindings.c input/bindings.c platform/nx_log.c $SDL_LIBS -pthread -o "$OUT/$name" >"$log" 2>&1; then
            if "$OUT/$name" >>"$log" 2>&1; then report ok "$name" "$log"; else report fail "$name" "$log"; fi
        else
            report fail "$name" "$log"
        fi
    }
else
    wanted test_bindings && { echo "SDL3 not found (pkg-config sdl3 or Homebrew sdl3)" >"$OUT/test_bindings.log"; report skip test_bindings "$OUT/test_bindings.log"; }
fi

# Python: the recompiler's NSO loader
if [ "$HAVE_PY_LZ4" = 1 ]; then
    run_py test_nso tests/test_nso.py
else
    wanted test_nso && { echo "python3 or the lz4 package not found" >"$OUT/test_nso.log"; report skip test_nso "$OUT/test_nso.log"; }
fi

# Python: the recompiler's chunk files, written in parallel
if [ "$HAVE_PY_RECOMP" = 1 ]; then
    run_py test_gen tests/test_gen.py
else
    wanted test_gen && { echo "python3 or its lz4/capstone/unicorn packages not found" >"$OUT/test_gen.log"; report skip test_gen "$OUT/test_gen.log"; }
fi

printf '\n%d passed, %d failed, %d skipped\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" = 0 ]
