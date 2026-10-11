#!/bin/sh
# Preprocessor check: for every test program, jcc's own sources and the
# reference program, the syntax tree jcc builds from its own preprocessing
# must match the one it builds from clang's. Line and file information is
# part of the dump, so the line markers are checked too. Run from the
# repository root (make cpp-check does).
cd "$(dirname "$0")/.." || exit 1
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
pass=0
fail=0

# check <name> <std> <flags...>: compare the dumps of one file
check() {
    name=$1; std=$2; shift 2
    base=$(basename "$name" .c)
    if ! ./jcc -ast-dump "-std=$std" "$@" "$name" > "$tmp/$base.own" 2> "$tmp/$base.own.err"; then
        echo "FAIL $name: jcc could not compile it with its own preprocessor"
        sed 's/^/    /' "$tmp/$base.own.err" | head -5
        fail=$((fail + 1))
        return
    fi
    if ! cc -E "-std=$std" "$@" "$name" > "$tmp/$base.cc.c" 2>/dev/null; then
        echo "SKIP $name: cc cannot preprocess it"
        return
    fi
    # the preprocessed file still goes through jcc's preprocessor, which only has to honour its line markers
    ./jcc -ast-dump "-std=$std" "$@" "$tmp/$base.cc.c" > "$tmp/$base.ref" 2>/dev/null
    # columns depend on how each preprocessor spaces its output, so only lines are compared
    sed 's/<\([^<>]*:\)\{0,1\}\([0-9]*\):[0-9]*>/<\1\2>/g' "$tmp/$base.own" > "$tmp/$base.own.l"
    sed 's/<\([^<>]*:\)\{0,1\}\([0-9]*\):[0-9]*>/<\1\2>/g' "$tmp/$base.ref" > "$tmp/$base.ref.l"
    if cmp -s "$tmp/$base.own.l" "$tmp/$base.ref.l"; then
        echo "PASS $name"
        pass=$((pass + 1))
    else
        echo "FAIL $name: syntax tree differs from clang's preprocessing"
        diff "$tmp/$base.ref.l" "$tmp/$base.own.l" | head -10 | sed 's/^/    /'
        fail=$((fail + 1))
    fi
}

for src in tests/*.c; do
    check "$src" c90
done
for src in src/*.c y.tab.c; do
    check "$src" gnu89 -Isrc
done
for src in src/arch/arch.*.c; do
    check "$src" gnu89 -Isrc -DJCC_ARM64_MACOS_SYSROOT='"/"' -DJCC_ARM64_MACOS_RESOURCE_INC='"/"' \
        -DJCC_X86_64_LINUX_SYSROOT='"/"' -DJCC_X86_64_LINUX_GCC_DIR='"/"'
done
if [ -f "$HOME/src/pewter/pewter.c" ]; then
    check "$HOME/src/pewter/pewter.c" c90
fi
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
