#!/bin/sh
# The library: build tests/embed/embed.c, a host program, against libkcc.a
# and run it (it checks itself; see the file); then tests/embed/growth.c,
# which compiles units of every kind in a loop and checks that the heap
# does not grow. Run from the repository root (make test does). A libkcc.a
# built with -fsanitize=address gets the hosts linked the same way.
cd "$(dirname "$0")/.." || exit 1
[ -f libkcc.a ] || { echo "no libkcc.a: run make first"; exit 1; }
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
flags=""
if nm libkcc.a 2>/dev/null | grep -q __asan_init; then flags="-fsanitize=address"; fi
cc -std=c89 -Wall -Wextra -Werror -Isrc $flags -o "$tmp/embed" tests/embed/embed.c libkcc.a || { echo "FAIL embed: the host program did not build"; exit 1; }
"$tmp/embed" "$tmp" || exit 1
cc -std=c89 -Wall -Wextra -Werror -Isrc $flags -o "$tmp/growth" tests/embed/growth.c libkcc.a || { echo "FAIL growth: the host program did not build"; exit 1; }
"$tmp/growth"
