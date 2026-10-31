#!/bin/sh
# Set up what the x86_64 FreeBSD target needs on this machine: a sysroot
# with FreeBSD's headers, C runtime (crt*.o), static and shared libc,
# libgcc (compiler-rt) and the run-time linker, extracted from the release's
# base.txz into $X86_64_FREEBSD_SYSROOT (default
# ~/.local/share/jcc/x86_64-freebsd), so jcc can preprocess, compile and
# (with ld.lld, `brew install lld`) link for FreeBSD here. Needs curl and a
# tar that reads xz (bsdtar, as on macOS and FreeBSD, or GNU tar). Safe to rerun.
#
#   FREEBSD_VERSION=15.1 tools/sysroot-freebsd.sh
#
# FreeBSD keeps old binaries running on newer releases, not the reverse, so
# the default is the oldest supported release.
set -e
VERSION=${FREEBSD_VERSION:-14.5}
SYSROOT=${X86_64_FREEBSD_SYSROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/jcc/x86_64-freebsd}
URL=https://download.freebsd.org/releases/amd64/$VERSION-RELEASE/base.txz
CACHE=${XDG_CACHE_HOME:-$HOME/.cache}/jcc
mkdir -p "$CACHE"
txz=$CACHE/freebsd-$VERSION-amd64-base.txz

if [ ! -f "$txz" ]; then
    echo "downloading $URL"
    curl -fL --progress-bar -o "$txz.part" "$URL"
    mv "$txz.part" "$txz"
fi

echo "extracting the sysroot to $SYSROOT"
rm -rf "$SYSROOT"
mkdir -p "$SYSROOT"
# the headers, the C runtime and libraries a C program links against, the shared
# libraries those name and the run-time linker
tar -xf "$txz" -C "$SYSROOT" \
    './usr/include/*' './usr/lib/*.o' './usr/lib/*.a' './usr/lib/*.so' \
    './lib/*.so.*' './libexec/ld-elf.so.1'
echo "$VERSION" >"$SYSROOT/VERSION"
echo "done: FreeBSD $VERSION headers in $SYSROOT/usr/include, C runtime and libraries in $SYSROOT/usr/lib"
