#!/bin/sh
# The library: build tests/embed/embed.c, a host program, against libjcc.a
# and run it (it checks itself; see the file); then tests/embed/growth.c,
# which compiles units of every kind in a loop and checks that the heap
# does not grow. Run from the repository root (make test does). A libjcc.a
# built with -fsanitize=address gets the hosts linked the same way.
cd "$(dirname "$0")/.." || exit 1
[ -f libjcc.a ] || { echo "no libjcc.a: run make first"; exit 1; }
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
flags=""
if nm libjcc.a 2>/dev/null | grep -q __asan_init; then flags="-fsanitize=address"; fi
cc -std=c89 -Wall -Wextra -Werror -Isrc $flags -o "$tmp/embed" tests/embed/embed.c libjcc.a || { echo "FAIL embed: the host program did not build"; exit 1; }
"$tmp/embed" "$tmp" || exit 1
cc -std=c89 -Wall -Wextra -Werror -Isrc $flags -o "$tmp/growth" tests/embed/growth.c libjcc.a || { echo "FAIL growth: the host program did not build"; exit 1; }
"$tmp/growth"
