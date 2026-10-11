#!/bin/sh
# Self-hosting check. Stage 1 is ./jcc as built by the host compiler.
# Stage 2 is jcc built by stage 1; stage 3 is jcc built by stage 2. Every
# stage 3 object must be byte-identical to its stage 2 object (the two
# compilers are the same program, so they must agree), and stage 2 must
# pass tests/run.sh. Run from the repository root (make selfhost does).
# The backends need each target's default header locations as macros
# (tests/target-defs.sh; make selfhost passes the Makefile's values). Both
# stages get the same values, so the objects stay comparable.
# `sh tests/selfhost.sh amalgamation` does the same with one file: each
# stage is dist/jcc.c (make amalgamation) and the driver, so jcc compiles
# the whole library in one translation unit. JCCFLAGS (say -O1) is passed
# to every compile of stages 2 and 3, so the optimizer builds jcc too.
cd "$(dirname "$0")/.." || exit 1
mode=${1:-sources}
. tests/target-defs.sh
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
[ -f y.tab.c ] || { echo "no y.tab.c: run make first"; exit 1; }
[ "$mode" != amalgamation ] || [ -f dist/jcc.c ] || { echo "no dist/jcc.c: run make amalgamation first"; exit 1; }

# build <compiler> <outdir>: compile every source of jcc with <compiler>, then
# link the driver against the library made from the rest, as the Makefile does;
# in amalgamation mode the library is the one file
build() {
    mkdir -p "$2"
    if [ "$mode" = amalgamation ]; then
        "$1" $JCCFLAGS -std=gnu89 $TARGET_DEFS -c -o "$2/jcc.o" dist/jcc.c || return 1
        "$1" $JCCFLAGS -std=gnu89 -c -o "$2/main.o" src/main.c || return 1
        cc -o "$2/jcc" "$2/main.o" "$2/jcc.o"
        return
    fi
    for src in src/*.c y.tab.c; do
        name=$(basename "$src" .c)
        "$1" $JCCFLAGS -std=gnu89 -c -o "$2/$name.o" "$src" || return 1
    done
    for src in src/arch/arch.*.c; do
        name=$(basename "$src" .c)
        # the four -D values contain spaces only if the paths do; word splitting is intended
        "$1" $JCCFLAGS -std=gnu89 -Isrc $TARGET_DEFS -c -o "$2/$name.o" "$src" || return 1
    done
    for src in src/as/*.c; do
        name=$(basename "$src" .c)
        "$1" $JCCFLAGS -std=gnu89 -Isrc -c -o "$2/as.$name.o" "$src" || return 1
    done
    rm -f "$2/libjcc.a"
    ar rcs "$2/libjcc.a" $(ls "$2"/*.o | grep -v '/main\.o$') || return 1
    cc -o "$2/jcc" "$2/main.o" "$2/libjcc.a"
}

echo "stage 2: building jcc ($mode) with ./jcc"
build ./jcc "$tmp/stage2" || { echo "FAIL: stage 2 did not build"; exit 1; }
echo "stage 3: building jcc with stage 2"
build "$tmp/stage2/jcc" "$tmp/stage3" || { echo "FAIL: stage 3 did not build"; exit 1; }

fail=0
for obj in "$tmp"/stage2/*.o; do
    name=$(basename "$obj")
    if ! cmp -s "$obj" "$tmp/stage3/$name"; then
        echo "FAIL: $name differs between stage 2 and stage 3"
        fail=1
    fi
done
[ "$fail" -eq 0 ] && echo "stage 2 and stage 3 objects are identical"

echo "running tests/run.sh with stage 2"
if JCC="$tmp/stage2/jcc" sh tests/run.sh >"$tmp/run.out" 2>&1; then
    tail -1 "$tmp/run.out"
else
    cat "$tmp/run.out"
    fail=1
fi
[ "$fail" -eq 0 ] && echo "SELFHOST OK"
exit "$fail"
