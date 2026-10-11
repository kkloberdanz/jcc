#!/bin/sh
# The library's files must not define the same static name twice, since
# the amalgamation (tools/amalgamate.sh) compiles them as one translation
# unit, where the second definition is an error, or worse, silently the
# same object (a tentative `static T x;` in one file merges with `static
# T x = v;` in another). This catches a regrowth early and cheaply: every
# library source is compiled on its own here (obj/*.o may be LTO bitcode,
# which nm cannot read), then nm lists each object's local symbols and
# any name defined in two objects fails. A static local inside a
# function (`f.x`) and the compiler's own literals are left out. Types,
# macros and enumerators are not symbols, so the amalgamation build
# (tests/amalgamation.sh) remains the full check. Run from the repository
# root (make statics and make verify do).
cd "$(dirname "$0")/.." || exit 1
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
defs='-DKCC_ARM64_MACOS_SYSROOT="" -DKCC_ARM64_MACOS_RESOURCE_INC="" -DKCC_X86_64_LINUX_SYSROOT="" -DKCC_X86_64_LINUX_GCC_DIR=""'
[ -f y.tab.c ] || { echo "no y.tab.c: run make first"; exit 1; }
for src in src/*.c src/arch/arch.*.c src/as/*.c y.tab.c; do
    [ "$src" != src/main.c ] || continue
    obj="$tmp/$(echo "$src" | tr / .).o"
    cc -std=gnu89 -O0 -Isrc $defs -c -o "$obj" "$src" || { echo "FAIL: $src does not compile"; exit 1; }
    # local symbols are the lowercase letters in nm's output on both GNU and BSD nm; ltmpN and lJTIn_m are clang's
    nm "$obj" | awk -v f="$src" '$2 ~ /^[a-z]$/ && $3 !~ /\.|^_?ltmp[0-9]+$|^_?lJTI[0-9]+_[0-9]+$/ { sub(/^_/, "", $3); print $3, f }'
done | sort -u | awk '
    { n[$1]++; where[$1] = where[$1] " " $2 }
    END { for (k in n) if (n[k] > 1) { bad++; print k ":" where[k] } ; if (bad) { print "STATICS: " bad " names defined in more than one file"; exit 1 } ; print "STATICS OK" }'
