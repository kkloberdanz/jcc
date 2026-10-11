#!/bin/sh
# Fetch GCC's execute torture tests (gcc/testsuite/gcc.c-torture/execute in
# github.com/gcc-mirror/gcc) into $GCC_TORTURE (default
# ~/.local/share/jcc/gcc-torture) for tests/run-gcc-torture.sh. Only that
# directory is checked out: a blobless, sparse, shallow clone. Safe to rerun.
set -e
DIR=${GCC_TORTURE:-${XDG_DATA_HOME:-$HOME/.local/share}/jcc/gcc-torture}
SUB=gcc/testsuite/gcc.c-torture/execute
if [ -d "$DIR/.git" ]; then
    git -C "$DIR" pull -q --ff-only
else
    git clone -q --depth 1 --filter=blob:none --sparse https://github.com/gcc-mirror/gcc "$DIR"
    git -C "$DIR" sparse-checkout set --no-cone "$SUB"
fi
echo "done: $(ls "$DIR/$SUB"/*.c | wc -l | tr -d ' ') tests in $DIR/$SUB"
