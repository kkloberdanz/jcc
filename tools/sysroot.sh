#!/bin/sh
# Set up what the x86_64 Linux target needs on this machine:
#   - a sysroot with Alpine's musl headers, gcc's own headers and libraries
#     and musl's C runtime (crt*.o, libc.a), copied out of Docker into
#     $X86_64_SYSROOT (default ~/.local/share/jcc/x86_64-linux-musl), so jcc can
#     preprocess, compile and (with ld.lld, `brew install lld`) link
#     statically for Linux here;
#   - a Docker image, jcc-x86_64-linux, with gcc, musl-dev, curl and procps (pewter's test uses ps -p), which
#     tests/run-x86_64.sh uses to link and run what jcc produced.
# Needs Docker running. Safe to rerun.
set -e
SYSROOT=${X86_64_SYSROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/jcc/x86_64-linux-musl}
docker info >/dev/null 2>&1 || { echo "docker is not running" >&2; exit 1; }

echo "building image jcc-x86_64-linux"
printf 'FROM alpine:3.20\nRUN apk add --no-cache gcc musl-dev curl procps\n' | docker build --platform linux/amd64 -q -t jcc-x86_64-linux - >/dev/null

echo "copying the sysroot to $SYSROOT"
rm -rf "$SYSROOT"
mkdir -p "$SYSROOT/usr/lib"
docker run --rm --platform linux/amd64 -v "$SYSROOT:/out" jcc-x86_64-linux sh -c '
    cp -r /usr/include /out/usr/ && cp -r /usr/lib/gcc /out/usr/lib/ &&
    cp /usr/lib/*crt*.o /usr/lib/*.a /out/usr/lib/ &&
    mkdir -p /out/lib && cp /lib/ld-musl-x86_64.so.1 /out/lib/ && ln -sf ../../lib/ld-musl-x86_64.so.1 /out/usr/lib/libc.so'
gccdir=$(ls -d "$SYSROOT"/usr/lib/gcc/x86_64-alpine-linux-musl/*/ | head -1)
echo "done: headers in $SYSROOT/usr/include, gcc in ${gccdir%/}, C runtime in $SYSROOT/usr/lib"
