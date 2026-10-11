#!/bin/sh
# Fetch the c-testsuite conformance tests (github.com/c-testsuite/c-testsuite)
# into $C_TESTSUITE (default ~/.local/share/kcc/c-testsuite) for
# tests/run-c-testsuite.sh. Safe to rerun.
set -e
SUITE=${C_TESTSUITE:-${XDG_DATA_HOME:-$HOME/.local/share}/kcc/c-testsuite}
if [ -d "$SUITE/.git" ]; then
    git -C "$SUITE" pull -q --ff-only
else
    git clone -q --depth 1 https://github.com/c-testsuite/c-testsuite "$SUITE"
fi
echo "done: $(ls "$SUITE"/tests/single-exec/*.c | wc -l | tr -d ' ') tests in $SUITE/tests/single-exec"
