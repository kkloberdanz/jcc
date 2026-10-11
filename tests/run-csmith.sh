#!/bin/sh
# Differential random testing: Csmith (built by tools/csmith.sh) generates N
# programs (default 100) from seeds SEED+1 .. SEED+N (SEED defaults to the
# time, and is printed, so a run can be repeated); each is built with the
# host compiler and with kcc, and both binaries must print the same
# checksum. A program the reference build cannot finish in 10 seconds is
# skipped. A failing program is kept as csmith-fail/pSEED.c, and
# `csmith --seed SEED` regenerates it. With `x86_64` as the argument the
# programs are cross compiled here and run in Docker. Programs are built
# and run JOBS at a time (default: the number of processors).
# Run from the repository root.
cd "$(dirname "$0")/.." || exit 1
KCC=${KCC:-./kcc}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
CSMITH=${CSMITH:-${XDG_DATA_HOME:-$HOME/.local/share}/kcc/csmith}
[ -x "$CSMITH/bin/csmith" ] || { echo "no csmith at $CSMITH: run tools/csmith.sh"; exit 2; }
N=${N:-100}
SEED=${SEED:-$(date +%s)}
mode=${1:-native}
if [ "$mode" = x86_64 ]; then
    SYSROOT=${X86_64_SYSROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/kcc/x86_64-linux-musl}
    docker info >/dev/null 2>&1 || { echo "docker is not running"; exit 2; }
    X="$KCC --target=x86_64-linux-musl -isysroot $SYSROOT"
else
    X="$KCC"
fi
if command -v timeout >/dev/null 2>&1; then T="timeout 10"; else T=""; fi
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin" "$tmp/res"
echo "csmith ($mode): $N programs from seed $SEED"

# one seed: generate, build and run the reference, build with kcc; the outcome is a file under res/
cat >"$tmp/compile1.sh" <<'EOF'
seed=$1; name=p$seed
"$CSMITH/bin/csmith" --seed $seed -o "$tmp/$name.c" >/dev/null 2>&1 || { echo "csmith failed for seed $seed" >"$tmp/res/$name.abort"; exit 0; }
cc -std=gnu89 -w -O0 -I "$CSMITH/include" -o "$tmp/ref.$name" "$tmp/$name.c" || { echo "cc failed for seed $seed" >"$tmp/res/$name.abort"; exit 0; }
if ! $T "$tmp/ref.$name" >"$tmp/bin/$name.expected" 2>&1; then
    : >"$tmp/res/$name.skip"
elif $X -std=gnu89 -I "$CSMITH/include" -o "$tmp/bin/$name" "$tmp/$name.c" >"$tmp/$name.cc" 2>&1; then
    : >"$tmp/res/$name.built"
else
    echo "FAIL seed $seed: did not compile: $(grep -m1 "error" "$tmp/$name.cc" | cut -c1-120)" >"$tmp/res/$name.fail"
fi
rm -f "$tmp/ref.$name"
EOF
# one built program, run in its directory: the verdict goes to NAME.verdict
cat >"$tmp/bin/run1.sh" <<'EOF'
T=""; command -v timeout >/dev/null 2>&1 && T="timeout 10"
name=$1
if $T ./$name >$name.out 2>&1 && cmp -s $name.expected $name.out; then echo "PASS $name" >$name.verdict
else echo "FAIL seed ${name#p}: $(head -1 $name.out), reference $(head -1 $name.expected)" >$name.verdict; fi
EOF
export tmp CSMITH X T
i=0
while [ $i -lt "$N" ]; do i=$((i + 1)); echo $((SEED + i)); done | xargs -P "$JOBS" -n 1 sh "$tmp/compile1.sh"
if ls "$tmp"/res/*.abort >/dev/null 2>&1; then cat "$tmp"/res/*.abort; exit 2; fi
skip=$(ls "$tmp/res" | grep -c '\.skip$')
built=$(ls "$tmp/res" | sed -n 's/\.built$//p' | tr '\n' ' ')
cat "$tmp"/res/*.fail >"$tmp/run.out" 2>/dev/null
if [ -n "$built" ]; then
    if [ "$mode" = x86_64 ]; then
        docker run --rm --platform linux/amd64 -v "$tmp/bin:/w" kcc-x86_64-linux \
            sh -c 'cd /w && printf "%s\n" $1 | xargs -P "$2" -n 1 sh ./run1.sh' sh "$built" "$JOBS"
    else
        (cd "$tmp/bin" && printf '%s\n' $built | xargs -P "$JOBS" -n 1 sh ./run1.sh)
    fi
    cat "$tmp"/bin/*.verdict >>"$tmp/run.out"
fi
grep "^FAIL" "$tmp/run.out" | while IFS= read -r line; do
    seed=${line#FAIL seed }; seed=${seed%%:*}
    mkdir -p csmith-fail && cp "$tmp/p$seed.c" "csmith-fail/p$seed.c"
    echo "$line"
done
fail=$(grep -c "^FAIL" "$tmp/run.out")
pass=$(grep -c "^PASS" "$tmp/run.out")
echo "csmith ($mode): $pass passed, $fail failed, $skip skipped (seed $SEED)"
[ "$fail" -eq 0 ]
