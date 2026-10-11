#!/bin/sh
# Run the c-testsuite conformance tests (github.com/c-testsuite/c-testsuite,
# fetched by tools/c-testsuite.sh): every tests/single-exec/*.c must build,
# exit 0 and print exactly its .expected file. With `x86_64` as the argument
# the tests are cross compiled and linked here and run in Docker. Tests
# tagged c99 or c11 are reported separately: kcc is a C89 compiler and
# passes them only where the feature is a common extension. Failures
# listed in tests/c-testsuite.xfail (name, then why) are expected and do
# not fail the run; one that starts passing is reported as XPASS so the
# list can be pruned; tests/c-testsuite.x86_64-linux.xfail is the x86_64
# target's own list. An unexpected c89 failure fails the run; c99 ones
# are reported only. Tests are compiled and run JOBS at a time (default:
# the number of processors).
# Run from the repository root.
cd "$(dirname "$0")/.." || exit 1
KCC=${KCC:-./kcc}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
XFAIL=tests/c-testsuite.xfail
SUITE=${C_TESTSUITE:-${XDG_DATA_HOME:-$HOME/.local/share}/kcc/c-testsuite}
[ -d "$SUITE/tests/single-exec" ] || { echo "no suite at $SUITE: run tools/c-testsuite.sh"; exit 2; }
mode=${1:-native}
if [ "$mode" = x86_64 ]; then
    SYSROOT=${X86_64_SYSROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/kcc/x86_64-linux-musl}
    docker info >/dev/null 2>&1 || { echo "docker is not running"; exit 2; }
    X="$KCC --target=x86_64-linux-musl -isysroot $SYSROOT"
    XFAIL="$XFAIL tests/c-testsuite.x86_64-linux.xfail"
else
    X="$KCC"
fi
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin" "$tmp/res"

# one test: build it with kcc; the outcome is a file under res/, its standard beside the binary
cat >"$tmp/compile1.sh" <<'EOF'
src=$1; name=$(basename "$src" .c)
if grep -q "^c89$" "$src.tags"; then std=c89; else std=c99; fi
echo "$std" >"$tmp/bin/$name.std"
if $X -std=gnu89 -o "$tmp/bin/$name" "$src" >"$tmp/$name.cc" 2>&1; then
    cp "$src.expected" "$tmp/bin/$name.expected"
    : >"$tmp/res/$name.built"
else
    echo "FAIL $name ($std): did not compile: $(grep -m1 "error" "$tmp/$name.cc" | cut -c1-120)" >"$tmp/res/$name.fail"
fi
EOF
# one built test, run in its directory: the verdict goes to NAME.verdict
cat >"$tmp/bin/run1.sh" <<'EOF'
name=$1; std=$(cat $name.std)
if ./$name >$name.out 2>&1 && cmp -s $name.expected $name.out; then echo "PASS $name ($std)" >$name.verdict
else { echo "FAIL $name ($std): exit $? or output differs"; diff $name.expected $name.out 2>/dev/null | head -5 | sed "s/^/    /"; } >$name.verdict; fi
EOF
export tmp X
ls "$SUITE"/tests/single-exec/*.c | xargs -P "$JOBS" -n 1 sh "$tmp/compile1.sh"
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
pass=0; fail=0; pass99=0; fail99=0; xfail=0; xpass=0
while IFS= read -r line; do
    name=${line#* }; name=${name%% *}
    std=${line#*(}; std=${std%%)*}
    expected=$(cat $XFAIL 2>/dev/null | grep -c "^$name ")
    case "$line" in
        PASS*)
            if [ "$expected" -gt 0 ]; then echo "XPASS $name ($std): remove it from $XFAIL"; xpass=$((xpass + 1))
            elif [ "$std" = c89 ]; then pass=$((pass + 1)); else pass99=$((pass99 + 1)); fi ;;
        FAIL*)
            if [ "$expected" -gt 0 ]; then echo "X$line"; xfail=$((xfail + 1))
            else echo "$line"; if [ "$std" = c89 ]; then fail=$((fail + 1)); else fail99=$((fail99 + 1)); fi; fi ;;
        *) echo "$line" ;;
    esac
done <"$tmp/run.out"
echo "c89: $pass passed, $fail failed; c99 and later: $pass99 passed, $fail99 failed; $xfail expected failures, $xpass unexpected passes"
[ "$fail" -eq 0 ]
