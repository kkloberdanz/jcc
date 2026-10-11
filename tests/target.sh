#!/bin/sh
# The --target switch: every backend is in the one binary. These checks need
# no sysroot (-nostdinc) and no Docker; the object-format checks need the
# host cc to be clang, whose integrated assembler knows both targets. Run
# from the repository root (make test does).
cd "$(dirname "$0")/.." || exit 1
JCC=${JCC:-./jcc}
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
fail=0
ok() { echo "PASS $1"; }
bad() { echo "FAIL $1"; fail=1; }

ARM=arm64-apple-macos
X86=x86_64-linux-musl
printf 'int main(void) { return 0; }\n' >"$tmp/t.c"
printf '#ifdef __linux__\nLINUX\n#endif\n#ifdef __APPLE__\nAPPLE\n#endif\n' >"$tmp/m.c"

# 1. listing and errors
list=$("$JCC" --print-targets) || bad "--print-targets exits 0"
case $list in *"$ARM"*"$X86"*) ok "--print-targets lists both targets";; *) bad "--print-targets lists both targets";; esac
[ "$("$JCC" --target=help)" = "$list" ] && ok "--target=help lists them" || bad "--target=help lists them"
host=$(printf '%s\n' "$list" | sed -n 's/^  \([^ ]*\) (host).*/\1/p')
[ -n "$host" ] && ok "the host target is marked" || bad "the host target is marked"
if "$JCC" --target=bogus -nostdinc -fsyntax-only "$tmp/t.c" 2>"$tmp/err"; then bad "--target=bogus is rejected"
elif grep -q "unknown target 'bogus'" "$tmp/err"; then ok "--target=bogus is rejected"; else bad "--target=bogus is rejected"; fi

# 2. each backend's assembly, and the alias spellings
"$JCC" --target=$X86 -nostdinc -S -o "$tmp/x86.s" "$tmp/t.c" && grep -q "GNU-stack" "$tmp/x86.s" \
    && ok "x86_64 assembly" || bad "x86_64 assembly"
"$JCC" --target=$ARM -nostdinc -S -o "$tmp/arm.s" "$tmp/t.c" && grep -q "^_main:" "$tmp/arm.s" \
    && ok "arm64 assembly" || bad "arm64 assembly"
"$JCC" -target aarch64-apple-darwin -nostdinc -S -o "$tmp/alias.s" "$tmp/t.c" && cmp -s "$tmp/arm.s" "$tmp/alias.s" \
    && ok "-target aarch64-apple-darwin is arm64" || bad "-target aarch64-apple-darwin is arm64"
"$JCC" --target amd64 -nostdinc -S -o "$tmp/alias2.s" "$tmp/t.c" && cmp -s "$tmp/x86.s" "$tmp/alias2.s" \
    && ok "--target amd64 is x86_64" || bad "--target amd64 is x86_64"

# 3. the preprocessor follows the target
m=$("$JCC" -E --target=$X86 -nostdinc "$tmp/m.c" | grep -v '^#')
[ "$(echo $m)" = LINUX ] && ok "-E --target=$X86 predefines __linux__" || bad "-E --target=$X86 predefines __linux__ (got: $m)"
m=$("$JCC" -E --target=$ARM -nostdinc "$tmp/m.c" | grep -v '^#')
[ "$(echo $m)" = APPLE ] && ok "-E --target=$ARM predefines __APPLE__" || bad "-E --target=$ARM predefines __APPLE__ (got: $m)"

# 4. object files, with clang's integrated assembler
if cc --version 2>/dev/null | grep -q clang; then
    "$JCC" --target=$X86 -nostdinc -c -o "$tmp/x86.o" "$tmp/t.c" \
        && [ "$(od -An -tx1 -N4 "$tmp/x86.o" | tr -d ' ')" = 7f454c46 ] && ok "x86_64 ELF object" || bad "x86_64 ELF object"
    "$JCC" --target=$ARM -nostdinc -c -o "$tmp/arm.o" "$tmp/t.c" \
        && [ "$(od -An -tx1 -N4 "$tmp/arm.o" | tr -d ' ')" = cffaedfe ] && ok "arm64 Mach-O object" || bad "arm64 Mach-O object"
    # linking for Linux here needs ld.lld and musl's runtime in the sysroot; for macOS it is refused
    if [ "$host" = "$X86" ]; then
        if "$JCC" --target=$ARM -nostdinc -o "$tmp/cross" "$tmp/t.c" 2>"$tmp/err"; then bad "linking for $ARM is refused"
        elif grep -q "cannot link for $ARM" "$tmp/err"; then ok "linking for $ARM is refused"; else bad "linking for $ARM is refused"; fi
    elif command -v ld.lld >/dev/null 2>&1 && [ -f "${X86_64_SYSROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/jcc/x86_64-linux-musl}/usr/lib/libc.a" ]; then
        "$JCC" --target=$X86 -nostdinc -o "$tmp/cross" "$tmp/t.c" \
            && [ "$(od -An -tx1 -j16 -N2 "$tmp/cross" | tr -d ' ')" = 0200 ] && ok "static x86_64 ELF executable linked here" \
            || bad "static x86_64 ELF executable linked here"
        "$JCC" --target=$X86 -nostdinc -dynamic -o "$tmp/cross-dyn" "$tmp/t.c" \
            && grep -q ld-musl-x86_64 "$tmp/cross-dyn" && ok "dynamic x86_64 ELF executable linked here (-dynamic)" \
            || bad "dynamic x86_64 ELF executable linked here (-dynamic)"
    else
        echo "SKIP cross link: needs ld.lld and tools/sysroot.sh"
    fi
    "$JCC" --target=$host -nostdinc -o "$tmp/native" "$tmp/t.c" && "$tmp/native" \
        && ok "--target=$host builds and runs" || bad "--target=$host builds and runs"
else
    echo "SKIP object format checks: cc is not clang"
fi

[ "$fail" -eq 0 ] && echo "TARGET OK"
exit "$fail"
