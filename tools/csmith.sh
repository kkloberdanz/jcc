#!/bin/sh
# Build Csmith (github.com/csmith-project/csmith), the random C program
# generator, into $CSMITH (default ~/.local/share/kcc/csmith) for
# tests/run-csmith.sh: the generator at $CSMITH/bin/csmith and its runtime
# header csmith.h under $CSMITH/include. Needs cmake and m4. Safe to rerun.
set -e
DIR=${CSMITH:-${XDG_DATA_HOME:-$HOME/.local/share}/kcc/csmith}
if [ -d "$DIR/src/.git" ]; then
    git -C "$DIR/src" pull -q --ff-only
else
    mkdir -p "$DIR"
    git clone -q --depth 1 https://github.com/csmith-project/csmith "$DIR/src"
fi
cmake -S "$DIR/src" -B "$DIR/build" -DCMAKE_INSTALL_PREFIX="$DIR" -DCMAKE_BUILD_TYPE=Release >"$DIR/build.log" 2>&1
cmake --build "$DIR/build" --target install -j 8 >>"$DIR/build.log" 2>&1
"$DIR/bin/csmith" --version | head -1
echo "done: $DIR/bin/csmith, header $(find "$DIR/include" -name csmith.h | head -1)"
