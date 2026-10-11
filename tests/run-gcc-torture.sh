#!/bin/sh
# Run GCC's execute torture tests (gcc.c-torture/execute, fetched by
# tools/gcc-torture.sh): each is a single file that exits 0 and prints
# nothing, or aborts, when built correctly. The host compiler decides which
# apply: a test it does not accept as gnu89, or that fails when it built it,
# is skipped (nested functions, vector types, GCC-only builtins); its verdict
# and output are cached under $GCC_TORTURE/ref. Every other test must build
# with kcc and behave the same. Failures listed in tests/gcc-torture.xfail
# (name, then why), or in the target's own list (tests/gcc-torture.arm64-macos.xfail,
# tests/gcc-torture.x86_64-linux.xfail), are expected and do not fail the
# run; one that starts passing is reported as XPASS so the list can be
# pruned. With `x86_64` as the argument the tests are cross compiled here
# and run in Docker. Tests are compiled and run JOBS at a time (default:
# the number of processors).
# Run from the repository root.
cd "$(dirname "$0")/.." || exit 1
KCC=${KCC:-./kcc}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
ROOT=${GCC_TORTURE:-${XDG_DATA_HOME:-$HOME/.local/share}/kcc/gcc-torture}
SUITE=$ROOT/gcc/testsuite/gcc.c-torture/execute
REF=$ROOT/ref
XFAIL=tests/gcc-torture.xfail
[ -d "$SUITE" ] || { echo "no suite at $SUITE: run tools/gcc-torture.sh"; exit 2; }
mode=${1:-native}
if [ "$mode" = x86_64 ]; then
    XFAIL="$XFAIL tests/gcc-torture.x86_64-linux.xfail"
    SYSROOT=${X86_64_SYSROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/kcc/x86_64-linux-musl}
    docker info >/dev/null 2>&1 || { echo "docker is not running"; exit 2; }
    X="$KCC --target=x86_64-linux-musl -isysroot $SYSROOT"
else
    X="$KCC"
    XFAIL="$XFAIL tests/gcc-torture.arm64-macos.xfail"
fi
if command -v timeout >/dev/null 2>&1; then T="timeout 10"; else T=""; fi
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin" "$tmp/res" "$REF"

# one test: the host compiler's verdict, cached as ref/NAME.out when the test
# applies, then the kcc build; the outcome is a file under res/
cat >"$tmp/compile1.sh" <<'EOF'
src=$1; name=$(basename "$src" .c)
if [ ! -f "$REF/$name.out" ] && [ ! -f "$REF/$name.skip" ]; then
    if cc -std=gnu89 -w -O0 -o "$tmp/ref.$name" "$src" -lm >/dev/null 2>&1 \
        && (cd "$tmp" && $T "./ref.$name" >"$tmp/ref.$name.out" 2>&1); then
        mv "$tmp/ref.$name.out" "$REF/$name.out"
    else
        : >"$REF/$name.skip"
    fi
    rm -f "$tmp/ref.$name" "$tmp/ref.$name.out"
fi
if [ -f "$REF/$name.skip" ]; then
    : >"$tmp/res/$name.skip"
elif $X -std=gnu89 -o "$tmp/bin/$name" "$src" -lm >"$tmp/$name.cc" 2>&1; then
    cp "$REF/$name.out" "$tmp/bin/$name.expected"
    : >"$tmp/res/$name.built"
else
    echo "FAIL $name: did not compile: $(grep -m1 "error" "$tmp/$name.cc" | cut -c1-120)" >"$tmp/res/$name.fail"
fi
EOF
# one built test, run in its directory: the verdict goes to NAME.verdict
cat >"$tmp/bin/run1.sh" <<'EOF'
T=""; command -v timeout >/dev/null 2>&1 && T="timeout 10"
name=$1
if $T ./$name >$name.out 2>&1 && cmp -s $name.expected $name.out; then echo "PASS $name" >$name.verdict
else echo "FAIL $name: exit $? or output differs" >$name.verdict; fi
EOF
export tmp REF X T
ls "$SUITE"/*.c | xargs -P "$JOBS" -n 1 sh "$tmp/compile1.sh"
skip=$(ls "$tmp/res" | grep -c '\.skip$')
built=$(ls "$tmp/res" | sed -n 's/\.built$//p' | tr '\n' ' ')
cat "$tmp"/res/*.fail >"$tmp/run.out" 2>/dev/null
# run them: here, or all at once in the container
if [ -n "$built" ]; then
    if [ "$mode" = x86_64 ]; then
        docker run --rm --platform linux/amd64 -v "$tmp/bin:/w" kcc-x86_64-linux \
            sh -c 'cd /w && printf "%s\n" $1 | xargs -P "$2" -n 1 sh ./run1.sh' sh "$built" "$JOBS"
    else
        (cd "$tmp/bin" && printf '%s\n' $built | xargs -P "$JOBS" -n 1 sh ./run1.sh)
    fi
    cat "$tmp"/bin/*.verdict >>"$tmp/run.out"
fi
# sort the verdicts against the expected failures
pass=0; fail=0; xfail=0; xpass=0
while IFS= read -r line; do
    name=${line#* }; name=${name%%:*}
    expected=$(cat $XFAIL 2>/dev/null | grep -c "^$name ")
    case "$line" in
        PASS*) if [ "$expected" -gt 0 ]; then echo "XPASS $name: remove it from $XFAIL"; xpass=$((xpass + 1)); else pass=$((pass + 1)); fi ;;
        FAIL*) if [ "$expected" -gt 0 ]; then echo "X$line"; xfail=$((xfail + 1)); else echo "$line"; fail=$((fail + 1)); fi ;;
    esac
done <"$tmp/run.out"
echo "gcc-torture ($mode): $pass passed, $fail failed, $xfail expected failures, $xpass unexpected passes, $skip skipped"
[ "$fail" -eq 0 ]
