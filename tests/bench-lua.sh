#!/bin/sh
# Build Lua 5.4.8 with a compiler and time tests/bench.lua with it. The
# plan records these numbers for each code generation step.
#   sh tests/bench-lua.sh            builds with ./kcc
#   KCC="cc -O0" sh tests/bench-lua.sh
# Needs tools/lua.sh to have run. Run from the repository root.
cd "$(dirname "$0")/.." || exit 1
KCC=${KCC:-./kcc}
DATA=${KCC_DATA:-${XDG_DATA_HOME:-$HOME/.local/share}/kcc}
LUA=$DATA/lua-5.4.8
[ -d "$LUA/src" ] || { echo "no Lua at $DATA: run tools/lua.sh"; exit 2; }
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
srcs=$(ls "$LUA"/src/*.c | grep -v "luac.c\|onelua.c")
# shellcheck disable=SC2086
$KCC -DLUA_USE_MACOSX -std=gnu99 -DLUA_COMPAT_5_3 -DLUA_USE_JUMPTABLE=0 -o "$tmp/lua" $srcs -lm -ldl \
    || { echo "FAIL: lua did not build with $KCC"; exit 1; }
echo "lua built with $KCC"
exec "$tmp/lua" tests/bench.lua
