#!/bin/sh
# The second real-program milestone: build Lua 5.4.8 with jcc and run its
# test suite. `sh tests/run-lua.sh` builds natively and runs the tests here;
# `sh tests/run-lua.sh x86_64` cross compiles and links a static Linux
# binary here and runs the tests in Docker. `jit` builds tests/jit/lua.c,
# a host linked with libjcc.a that compiles the interpreter in process,
# loads it into itself (Step 15) and calls its main, and runs the tests
# with that at -O0 and at -O1; `jit-x86_64` does the same on Linux in
# Docker, with the host and the amalgamation (make amalgamation) built
# there by gcc. Needs tools/lua.sh to have run.
# Run from the repository root.
cd "$(dirname "$0")/.." || exit 1
JCC=${JCC:-./jcc}
DATA=${JCC_DATA:-${XDG_DATA_HOME:-$HOME/.local/share}/jcc}
LUA=$DATA/lua-5.4.8
TESTS=$DATA/lua-5.4.8-tests
[ -d "$LUA/src" ] && [ -d "$TESTS" ] || { echo "no Lua at $DATA: run tools/lua.sh"; exit 2; }
mode=${1:-native}
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

# Lua is C99; LUA_USE_JUMPTABLE=0 avoids the GNU computed goto its VM
# otherwise uses under __GNUC__. lua.c is the interpreter; luac.c and
# onelua.c are separate programs.
srcs=$(ls "$LUA"/src/*.c | grep -v "luac.c\|onelua.c")

# _U=true selects the tests that need no auxiliary C libraries or special
# builds (the "basic" mode of Lua's suite). musl's setlocale accepts any
# locale name without changing the decimal point, which fails a locale
# test even with a gcc-built Lua, so on Linux non-C locales are reported as
# unavailable, as they are.
LINUX_PRELUDE='_U=true; local sl = os.setlocale; os.setlocale = function(l, c) if l and l ~= "C" and l ~= "" then return nil end return sl(l, c) end'

# the interpreter jit-compiled in process: <how to run lua-jit> per level, judged as below
if [ "$mode" = jit ] || [ "$mode" = jit-x86_64 ]; then
    cp -r "$TESTS" "$tmp/testes"
    if [ "$mode" = jit-x86_64 ]; then
        [ -f dist/jcc.c ] || { echo "no dist/jcc.c: run make amalgamation first"; exit 1; }
        docker info >/dev/null 2>&1 || { echo "docker is not running"; exit 2; }
        cp dist/jcc.c dist/jcc.h tests/jit/lua.c "$tmp/"
        cp -r "$LUA/src" "$tmp/luasrc"
        echo "building the host in Docker"
        docker run --rm --platform linux/amd64 -v "$tmp:/w" jcc-x86_64-linux sh -c '
            cd /w &&
            gcc -std=gnu89 -O1 -w -c jcc.c \
                -DJCC_ARM64_MACOS_SYSROOT=\"/\" -DJCC_ARM64_MACOS_RESOURCE_INC=\"/\" \
                -DJCC_X86_64_LINUX_SYSROOT=\"/\" -DJCC_X86_64_FREEBSD_SYSROOT=\"/\" \
                -DJCC_X86_64_LINUX_GCC_DIR=\"$(dirname $(gcc -print-libgcc-file-name))\" &&
            gcc -std=c89 -Wall -Wextra -Werror -o lua-jit lua.c jcc.o' || { echo "FAIL: the host did not build"; exit 1; }
    else
        [ -f libjcc.a ] || { echo "no libjcc.a: run make first"; exit 1; }
        cc -std=c89 -Wall -Wextra -Werror -Isrc -o "$tmp/lua-jit" tests/jit/lua.c libjcc.a || { echo "FAIL: the host did not build"; exit 1; }
    fi
    fail=0
    for level in 0 1; do
        echo "lua jit-compiled in process at -O$level ($mode)"
        if [ "$mode" = jit-x86_64 ]; then
            docker run --rm --platform linux/amd64 -v "$tmp:/w" -e LEVEL=$level -e PRELUDE="$LINUX_PRELUDE" jcc-x86_64-linux sh -c '
                cd /w/testes && /w/lua-jit -O $LEVEL /w/luasrc -e "$PRELUDE" all.lua' > "$tmp/out" 2>&1
            rc=$?
        else
            (cd "$tmp/testes" && "$tmp/lua-jit" -O $level "$LUA/src" -e "_U=true" all.lua) > "$tmp/out" 2>&1
            rc=$?
        fi
        tail -2 "$tmp/out"
        if [ "$rc" -ne 0 ] || ! grep -q "final OK" "$tmp/out"; then
            echo "FAIL: Lua's tests did not pass jit-compiled at -O$level ($mode)"
            grep -v "^\.*$" "$tmp/out" | tail -15
            fail=1
        fi
    done
    [ "$fail" -eq 0 ] && echo "LUA OK ($mode)"
    exit "$fail"
fi
if [ "$mode" = x86_64 ]; then
    SYSROOT=${X86_64_SYSROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/jcc/x86_64-linux-musl}
    docker info >/dev/null 2>&1 || { echo "docker is not running"; exit 2; }
    command -v ld.lld >/dev/null 2>&1 || { echo "no ld.lld: brew install lld"; exit 2; }
    X="$JCC --target=x86_64-linux-musl -isysroot $SYSROOT -DLUA_USE_LINUX"
else
    X="$JCC -DLUA_USE_MACOSX"
fi
echo "building lua ($mode)"
# shellcheck disable=SC2086
$X -std=gnu99 -DLUA_COMPAT_5_3 -DLUA_USE_JUMPTABLE=0 -o "$tmp/lua" $srcs -lm -ldl || { echo "FAIL: lua did not build"; exit 1; }
cp -r "$TESTS" "$tmp/testes"
if [ "$mode" = x86_64 ]; then
    docker run --rm --platform linux/amd64 -v "$tmp:/w" jcc-x86_64-linux sh -c '
        cd /w/testes && /w/lua -e "_U=true; local sl = os.setlocale; os.setlocale = function(l, c) if l and l ~= \"C\" and l ~= \"\" then return nil end return sl(l, c) end" all.lua' > "$tmp/out" 2>&1
    rc=$?
else
    (cd "$tmp/testes" && "$tmp/lua" -e "_U=true" all.lua) > "$tmp/out" 2>&1
    rc=$?
fi
tail -4 "$tmp/out"
if [ "$rc" -eq 0 ] && grep -q "final OK" "$tmp/out"; then
    echo "LUA OK ($mode)"
else
    echo "FAIL: Lua's tests did not pass ($mode)"
    grep -v "^\.*$" "$tmp/out" | tail -15
    exit 1
fi
