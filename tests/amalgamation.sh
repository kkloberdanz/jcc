#!/bin/sh
# The amalgamation (Step 19): dist/jcc.c and dist/jcc.h, the library as
# one file (make amalgamation). Three checks: the host compiler builds it
# with the Makefile's flags and the driver linked against it passes
# tests/run.sh; the embed hosts (tests/embed/) build against dist/ alone,
# with no -Isrc and no libjcc.a; and jcc builds it (tests/selfhost.sh in
# its one-file mode: stage 2 and stage 3 objects identical, tests/run.sh
# with stage 2). The backends' header locations come from the same
# variables as the Makefile's (tests/target-defs.sh; make
# test-amalgamation passes them). Run from the repository root.
cd "$(dirname "$0")/.." || exit 1
. tests/target-defs.sh
[ -f dist/jcc.c ] && [ -f dist/jcc.h ] || { echo "no dist/jcc.c: run make amalgamation first"; exit 1; }
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
flags="-Werror -Wall -Wextra -std=gnu89"

echo "host compiler: cc $flags -c dist/jcc.c"
cc $flags $TARGET_DEFS -c -o "$tmp/jcc.o" dist/jcc.c || { echo "FAIL: dist/jcc.c does not build"; exit 1; }
cc $flags -o "$tmp/jcc" src/main.c "$tmp/jcc.o" || { echo "FAIL: the driver does not link against it"; exit 1; }
JCC="$tmp/jcc" sh tests/run.sh >"$tmp/run.out" 2>&1 || { cat "$tmp/run.out"; echo "FAIL: tests/run.sh with the amalgamation"; exit 1; }
tail -1 "$tmp/run.out"

echo "the embed hosts against dist/ alone"
for host in embed growth; do
    cc -std=c89 -Wall -Wextra -Werror -Idist $TARGET_DEFS -o "$tmp/$host" "tests/embed/$host.c" dist/jcc.c \
        || { echo "FAIL: tests/embed/$host.c does not build against dist/jcc.c"; exit 1; }
done
"$tmp/embed" "$tmp" >"$tmp/embed.out" 2>&1 || { cat "$tmp/embed.out"; exit 1; }
tail -1 "$tmp/embed.out"
"$tmp/growth" || exit 1

sh tests/selfhost.sh amalgamation || exit 1
echo "AMALGAMATION OK"
