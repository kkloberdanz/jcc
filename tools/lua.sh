#!/bin/sh
# Fetch Lua 5.4.8 and its test suite into $KCC_DATA (default
# ~/.local/share/kcc) for tests/run-lua.sh. Safe to rerun.
set -e
DATA=${KCC_DATA:-${XDG_DATA_HOME:-$HOME/.local/share}/kcc}
mkdir -p "$DATA"
cd "$DATA"
if [ ! -d lua-5.4.8 ]; then
    curl -sL -o lua-5.4.8.tar.gz https://www.lua.org/ftp/lua-5.4.8.tar.gz
    tar -xzf lua-5.4.8.tar.gz && rm lua-5.4.8.tar.gz
fi
if [ ! -d lua-5.4.8-tests ]; then
    curl -sL -o lua-5.4.8-tests.tar.gz https://www.lua.org/tests/lua-5.4.8-tests.tar.gz
    tar -xzf lua-5.4.8-tests.tar.gz && rm lua-5.4.8-tests.tar.gz
fi
echo "done: $DATA/lua-5.4.8 and $DATA/lua-5.4.8-tests"
