#!/bin/sh
# ABI test: each tests/abi/NAME.callee.c and tests/abi/NAME.c (the caller,
# with main) is compiled by the system compiler and by jcc, and all four
# mixtures of the two objects are linked and run. Every mixture must exit
# with the same status and print the same output as the all-cc build, so
# jcc's calling convention, layout and alignment agree with the platform's
# compiler in both directions. With `x86_64` as the argument the jcc objects
# are cross compiled here and gcc's objects, the links and the runs happen
# in Docker (needs tools/sysroot.sh). Run from the repository root.
cd "$(dirname "$0")/.." || exit 1
JCC=${JCC:-./jcc}
mode=${1:-native}
if [ "$mode" = x86_64 ]; then
    SYSROOT=${X86_64_SYSROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/jcc/x86_64-linux-musl}
    [ -d "$SYSROOT/usr/include" ] || { echo "no sysroot at $SYSROOT: run tools/sysroot.sh"; exit 2; }
    docker info >/dev/null 2>&1 || { echo "docker is not running"; exit 2; }
    X="$JCC --target=x86_64-linux-musl -isysroot $SYSROOT"
else
    X="$JCC"
fi
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/w"
fail=0
names=""
# compile jcc's two objects for every pair here
for callee in tests/abi/*.callee.c; do
    name=$(basename "$callee" .callee.c)
    caller=tests/abi/$name.c
    [ -f "$caller" ] || { echo "FAIL $name: no $caller"; fail=$((fail + 1)); continue; }
    ok=1
    for side in caller callee; do
        if [ $side = caller ]; then src=$caller; else src=$callee; fi
        if ! $X -c -o "$tmp/w/$name.$side.jcc.o" "$src" 2>"$tmp/err"; then
            echo "FAIL $name: jcc failed on $src"; sed 's/^/    /' "$tmp/err"; ok=0
        fi
    done
    [ $ok = 1 ] || { fail=$((fail + 1)); continue; }
    cp "$caller" "$tmp/w/$name.caller.c"
    cp "$callee" "$tmp/w/$name.callee.c"
    [ -f "tests/abi/$name.h" ] && cp "tests/abi/$name.h" "$tmp/w/"
    names="$names $name"
done
# the system compiler's objects, the four links and the runs: here or in Docker
runner='
    cd "$1"; shift; fail=0
    for name in "$@"; do
        if ! cc -std=c89 -w -c -o $name.caller.cc.o $name.caller.c 2>/dev/null \
            || ! cc -std=c89 -w -c -o $name.callee.cc.o $name.callee.c 2>/dev/null; then
            echo "SKIP $name: cc does not accept it"; continue
        fi
        for mix in cc-cc cc-jcc jcc-cc jcc-jcc; do
            a=${mix%-*}; b=${mix#*-}
            if ! cc -o $name.$mix $name.caller.$a.o $name.callee.$b.o 2>$name.$mix.err; then
                echo "FAIL $name $mix: link failed"; sed "s/^/    /" $name.$mix.err | head -5; fail=$((fail + 1)); continue
            fi
            ./$name.$mix >$name.$mix.out 2>&1; echo $? >$name.$mix.status
        done
        [ -f $name.cc-cc.status ] || continue
        want=$(cat $name.cc-cc.status)
        for mix in cc-jcc jcc-cc jcc-jcc; do
            [ -f $name.$mix.status ] || continue
            got=$(cat $name.$mix.status)
            if [ "$got" != "$want" ]; then
                echo "FAIL $name $mix (caller-callee): exit status $got, cc-cc gives $want"
                diff $name.cc-cc.out $name.$mix.out | head -10 | sed "s/^/    /"; fail=$((fail + 1))
            elif ! cmp -s $name.cc-cc.out $name.$mix.out; then
                echo "FAIL $name $mix (caller-callee): output differs from cc-cc"
                diff $name.cc-cc.out $name.$mix.out | head -10 | sed "s/^/    /"; fail=$((fail + 1))
            else
                echo "PASS $name $mix"
            fi
        done
    done
    echo "ABI: $fail failed"; [ $fail -eq 0 ]'
if [ "$mode" = x86_64 ]; then
    docker run --rm --platform linux/amd64 -v "$tmp/w:/w" jcc-x86_64-linux sh -c "$runner" sh /w $names
else
    sh -c "$runner" sh "$tmp/w" $names
fi
[ $? -eq 0 ] && [ "$fail" -eq 0 ]
