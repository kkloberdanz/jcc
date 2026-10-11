#!/bin/sh
# The assembler's front end (Step 14, piece 1): jcc reads the assembly its
# own backends write and nothing else. The assembly of every test program,
# for both targets, must pass `jcc -fsyntax-only file.s`; lines outside the
# subset must be rejected with a diagnostic that names the line; and when
# the corpus tools/as-corpus.sh collects is present, every file in it is
# checked too. Run from the repository root (make test does).
cd "$(dirname "$0")/.." || exit 1
JCC=${JCC:-./jcc}
DATA=${JCC_DATA:-${XDG_DATA_HOME:-$HOME/.local/share}/jcc}
SYSROOT=${X86_64_SYSROOT:-$DATA/x86_64-linux-musl}
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
fail=0
X86="$JCC --target=x86_64-linux-musl -isysroot $SYSROOT"
[ -d "$SYSROOT/usr/include" ] || X86=""

# every test program's assembly is accepted
n=0
for src in tests/*.c tests/abi/*.c; do
    name=$(basename "$src" .c)
    for mode in arm64 x86_64; do
        if [ "$mode" = arm64 ]; then X="$JCC"; else X="$X86"; fi
        [ -n "$X" ] || continue
        $X -S -o "$tmp/$name.$mode.s" "$src" 2>/dev/null || continue
        n=$((n + 1))
        if ! $X -fsyntax-only "$tmp/$name.$mode.s" 2>"$tmp/err"; then
            echo "FAIL $name ($mode): its assembly is not in the assembler's subset"; head -5 "$tmp/err" | sed 's/^/    /'
            fail=$((fail + 1))
        fi
    done
done
echo "accepted: $n files"

# lines outside the subset are rejected, each with its line number
reject() {    # reject TARGET-FLAGS LINE-NUMBER MESSAGE
    if $1 -fsyntax-only "$tmp/bad.s" >"$tmp/err" 2>&1; then
        echo "FAIL rejection: accepted line $2 ($3)"; fail=$((fail + 1))
    elif ! grep -q "bad.s:$2:1: error: assembler: $3" "$tmp/err"; then
        echo "FAIL rejection: line $2 ($3), got:"; sed 's/^/    /' "$tmp/err"; fail=$((fail + 1))
    fi
}
printf '\t.text\n\tfrob x0, x1\n' >"$tmp/bad.s"; reject "$JCC" 2 "unsupported instruction form"
printf '\t.text\n\tmov x0, [x1]\n' >"$tmp/bad.s"; reject "$JCC" 2 "unsupported instruction form"
printf '\t.text\n\tldr x0, [x1, #8, lsl #3]\n' >"$tmp/bad.s"; reject "$JCC" 2 "cannot parse operand"
printf '\t.text\n_f:\n\t.weak _f\n' >"$tmp/bad.s"; reject "$JCC" 3 "unsupported directive"
printf '\t.data\n\tadd x0, x0, #1\n' >"$tmp/bad.s"; reject "$JCC" 2 "an instruction outside .text"
printf '\t.text\n\tmov x0, ~1\n' >"$tmp/bad.s"; reject "$JCC" 2 "unexpected character"
printf '\t.bss\n_x:\n\t.byte 1\n' >"$tmp/bad.s"; reject "$JCC" 3 "non-zero data in .bss"
if [ -n "$X86" ]; then
    printf '\t.text\n\tmovq %%rax, %%rbx, %%rcx\n' >"$tmp/bad.s"; reject "$X86" 2 "unsupported instruction form"
    printf '\t.text\n\tmovq 8(%%rax,%%rbx,3), %%rcx\n' >"$tmp/bad.s"; reject "$X86" 2 "cannot parse operand"
    printf '\t.text\n\tcall *8(%%rax)\n' >"$tmp/bad.s"; reject "$X86" 2 "cannot parse operand"
fi

# the collected corpus, when it is there
for mode in arm64 x86_64; do
    [ -d "$DATA/as-corpus/$mode" ] || continue
    if [ "$mode" = arm64 ]; then X="$JCC"; else X="$X86"; fi
    [ -n "$X" ] || continue
    count=0
    for f in "$DATA/as-corpus/$mode"/*.s; do
        count=$((count + 1))
        if ! $X -fsyntax-only "$f" 2>"$tmp/err"; then
            echo "FAIL corpus $(basename "$f") ($mode)"; head -3 "$tmp/err" | sed 's/^/    /'
            fail=$((fail + 1))
        fi
    done
    echo "corpus ($mode): $count files"
done

if [ "$fail" -eq 0 ]; then
    echo "AS OK"
else
    echo "AS: $fail failed"
    exit 1
fi
