#!/bin/sh
# Collect the assembly corpus for the assembler (Step 14): every .s kcc
# produces for the test suites, pewter, c-testsuite, the gcc torture
# tests, Lua, SQLite and kcc's own sources, for both targets, under
# DATA/as-corpus/<arch>/. A source kcc cannot compile is skipped. Needs
# the suites fetched by the other tools/*.sh scripts. Run from the
# repository root; sh tools/as-corpus.sh [arm64|x86_64] does one target.
cd "$(dirname "$0")/.." || exit 1
KCC=${KCC:-./kcc}
DATA=${KCC_DATA:-${XDG_DATA_HOME:-$HOME/.local/share}/kcc}
SYSROOT=${X86_64_SYSROOT:-$DATA/x86_64-linux-musl}
OUT=${AS_CORPUS:-$DATA/as-corpus}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
PEWTER=${PEWTER:-$HOME/src/pewter}
TARGET_DEFS=$(make -s -f - print <<'MK'
include Makefile
print:
	@echo $(TARGET_DEFS)
MK
)

for arch in ${1:-arm64 x86_64}; do
    case $arch in
        arm64) X="$KCC"; osdef=-DLUA_USE_MACOSX ;;
        x86_64) X="$KCC --target=x86_64-linux-musl -isysroot $SYSROOT"; osdef=-DLUA_USE_LINUX ;;
        *) echo "unknown target $arch"; exit 1 ;;
    esac
    rm -rf "$OUT/$arch"
    mkdir -p "$OUT/$arch"
    list=$(mktemp) || exit 1
    # each line: output-name<TAB>flags<TAB>source
    for f in tests/*.c tests/abi/*.c; do
        echo "$(basename "$f" .c)||$f"
    done >"$list"
    [ -f "$PEWTER/pewter.c" ] && echo "pewter||$PEWTER/pewter.c" >>"$list"
    for f in "$DATA"/c-testsuite/tests/single-exec/*.c; do
        echo "cts-$(basename "$f" .c)||$f"
    done >>"$list"
    for f in "$DATA"/gcc-torture/gcc/testsuite/gcc.c-torture/execute/*.c; do
        echo "torture-$(basename "$f" .c)|-std=gnu89|$f"
    done >>"$list"
    for f in "$DATA"/lua-5.4.8/src/*.c; do
        echo "lua-$(basename "$f" .c)|-std=gnu99 -DLUA_COMPAT_5_3 -DLUA_USE_JUMPTABLE=0 $osdef|$f"
    done >>"$list"
    for f in "$DATA"/sqlite-amalgamation-3530400/sqlite3.c "$DATA"/sqlite-amalgamation-3530400/shell.c; do
        echo "sqlite-$(basename "$f" .c)|-std=gnu99|$f"
    done >>"$list"
    for f in src/*.c src/arch/arch.*.c y.tab.c; do
        echo "kcc-$(basename "$f" .c)|-std=gnu89 -Isrc $TARGET_DEFS|$f"
    done >>"$list"
    echo "$arch: $(wc -l <"$list" | tr -d ' ') sources"
    export X OUT arch list
    seq "$(wc -l <"$list")" | xargs -P "$JOBS" -n 1 sh -c '
        IFS="|" read -r name flags src <<LINE
$(sed -n "${0}p" "$list")
LINE
        $X $flags -S -o "$OUT/$arch/$name.s" "$src" 2>/dev/null || rm -f "$OUT/$arch/$name.s"
    ' 2>/dev/null
    rm -f "$list"
    echo "$arch: $(ls "$OUT/$arch" | wc -l | tr -d ' ') assembled, $(cat "$OUT/$arch"/*.s | wc -l | tr -d ' ') lines"
done
