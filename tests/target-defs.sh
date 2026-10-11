# Sourced by the scripts that compile kcc's backends outside the Makefile
# (tests/selfhost.sh, tests/amalgamation.sh): sets TARGET_DEFS, the four
# -D macros naming each target's default header locations, from the
# variables of the same names, which default as in the Makefile (the make
# targets pass them, so a build from make and one from a script agree).
if [ "$(uname -s)" = Darwin ]; then
    : "${ARM64_MACOS_SYSROOT:=$(xcrun --show-sdk-path)}"
    : "${ARM64_MACOS_RESOURCE_INC:=$(cc -print-resource-dir)/include}"
    : "${X86_64_LINUX_SYSROOT:=${XDG_DATA_HOME:-$HOME/.local/share}/kcc/x86_64-linux-musl}"
else
    : "${ARM64_MACOS_SYSROOT:=/}"
    : "${ARM64_MACOS_RESOURCE_INC:=/}"
    : "${X86_64_LINUX_SYSROOT:=/}"
fi
if [ -z "$X86_64_LINUX_GCC_DIR" ]; then
    ver=$(ls -d "$X86_64_LINUX_SYSROOT"/usr/lib/gcc/x86_64-alpine-linux-musl/*/include 2>/dev/null | head -1)
    ver=${ver:-/13.2.1/include}; ver=${ver%/include}; ver=${ver##*/}
    X86_64_LINUX_GCC_DIR="=/usr/lib/gcc/x86_64-alpine-linux-musl/$ver"
fi
TARGET_DEFS="-DKCC_ARM64_MACOS_SYSROOT=\"$ARM64_MACOS_SYSROOT\" -DKCC_ARM64_MACOS_RESOURCE_INC=\"$ARM64_MACOS_RESOURCE_INC\" \
    -DKCC_X86_64_LINUX_SYSROOT=\"$X86_64_LINUX_SYSROOT\" -DKCC_X86_64_LINUX_GCC_DIR=\"$X86_64_LINUX_GCC_DIR\""
