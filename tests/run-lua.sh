#!/bin/sh
# The second real-program milestone: build Lua 5.4.8 with jcc and run its
# test suite. `sh tests/run-lua.sh` builds natively and runs the tests here;
# `sh tests/run-lua.sh x86_64` cross compiles and links a static Linux
# binary here and runs the tests in Docker. Needs tools/lua.sh to have run.
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
# _U=true selects the tests that need no auxiliary C libraries or special
# builds (the "basic" mode of Lua's suite). musl's setlocale accepts any
# locale name without changing the decimal point, which fails a locale
# test even with a gcc-built Lua, so on Linux non-C locales are reported as
# unavailable, as they are.
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
