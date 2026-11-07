#!/bin/sh
# In-process execution (Step 15 of the plan): build tests/jit/jit.c, a
# host that compiles C, loads it into itself and calls it, against
# libjcc.a and run it (it checks itself; see the file). `sh tests/jit.sh
# x86_64` runs it on x86_64 Linux instead: the amalgamation (make
# amalgamation) and the host are built with gcc inside the Docker image
# the x86_64 tests use, and run there. Run from the repository root
# (make test does the native run). A libjcc.a built with
# -fsanitize=address gets the host linked the same way.
cd "$(dirname "$0")/.." || exit 1
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
if [ "$1" = x86_64 ]; then
    [ -f dist/jcc.c ] || { echo "no dist/jcc.c: run make amalgamation first"; exit 1; }
    docker info >/dev/null 2>&1 || { echo "docker is not running"; exit 2; }
    cp dist/jcc.c dist/jcc.h tests/jit/jit.c "$tmp/"
    docker run --rm --platform linux/amd64 -v "$tmp:/w" jcc-x86_64-linux sh -c '
        cd /w &&
        gcc -std=gnu89 -O1 -w -c jcc.c \
            -DJCC_ARM64_MACOS_SYSROOT=\"/\" -DJCC_ARM64_MACOS_RESOURCE_INC=\"/\" \
            -DJCC_X86_64_LINUX_SYSROOT=\"/\" -DJCC_X86_64_FREEBSD_SYSROOT=\"/\" \
            -DJCC_X86_64_LINUX_GCC_DIR=\"$(dirname $(gcc -print-libgcc-file-name))\" &&
        gcc -std=c89 -Wall -Wextra -Werror -o jit jit.c jcc.o &&
        ./jit'
    exit $?
fi
[ -f libjcc.a ] || { echo "no libjcc.a: run make first"; exit 1; }
flags=""
if nm libjcc.a 2>/dev/null | grep -q __asan_init; then flags="-fsanitize=address"; fi
cc -std=c89 -Wall -Wextra -Werror -Isrc $flags -o "$tmp/jit" tests/jit/jit.c libjcc.a || { echo "FAIL jit: the host program did not build"; exit 1; }
"$tmp/jit"
