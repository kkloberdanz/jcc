#!/bin/sh
# The x86_64 Linux target, from this machine: with `kcc --target=x86_64-linux-musl`
# against the Linux sysroot, cross compile every test to an ELF object here
# (kcc -c writes the ELF object itself), then link, build
# the gcc references and run both inside Docker, comparing exit status and
# output as tests/run.sh does. With ld.lld installed (brew install lld) each
# test is also linked here, statically and (-dynamic) against musl's shared
# libc, and those binaries are run and compared too. `sh tests/run-x86_64.sh pewter` instead builds pewter.c (linked here
# when ld.lld is installed, else by gcc in the container) and runs its own
# end-to-end test as root in the container.
# Needs tools/sysroot.sh to have run. Run from the repository root.
cd "$(dirname "$0")/.." || exit 1
SYSROOT=${X86_64_SYSROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/kcc/x86_64-linux-musl}
[ -d "$SYSROOT/usr/include" ] || { echo "no sysroot at $SYSROOT: run tools/sysroot.sh"; exit 2; }
docker info >/dev/null 2>&1 || { echo "docker is not running"; exit 2; }
KCC=${KCC:-./kcc}
[ -x "$KCC" ] || { echo "no $KCC: run make first"; exit 1; }
# the compiler under test, aimed at Linux; -isysroot also moves gcc's own headers
X="$KCC --target=x86_64-linux-musl -isysroot $SYSROOT"
if command -v ld.lld >/dev/null 2>&1; then hostlink=1; else hostlink=0; echo "no ld.lld: linking only in Docker"; fi
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/work"
if [ "$1" = pewter ]; then
    src=${PEWTER:-$HOME/src/pewter}
    cp "$src/test.sh" "$tmp/work/"
    if [ "$hostlink" = 1 ]; then
        echo "cross compiling and linking pewter.c here"
        $X -o "$tmp/work/pewter" "$src/pewter.c" || exit 1
        link=true
    else
        echo "cross compiling pewter.c"
        $X -c -o "$tmp/work/pewter.o" "$src/pewter.c" || exit 1
        link="gcc -o pewter pewter.o"
    fi
    echo "testing in Docker (as root, so the chroot works)"
    docker run --rm --platform linux/amd64 -v "$tmp/work:/w" -e LINK="$link" kcc-x86_64-linux sh -c '
        cd /w && $LINK && ./pewter -h | head -1 && sh test.sh ./pewter 8089'
    exit $?
fi

# cross compile every test; a syntax-only test is checked here and goes no further
names=""
for src in tests/*.c; do
    name=$(basename "$src" .c)
    if head -5 "$src" | grep -q "kcc: syntax-only"; then
        if $X -fsyntax-only "$src"; then
            echo "PASS $name (syntax only)"
        else
            echo "FAIL $name: kcc rejected it"
        fi
        continue
    fi
    if $X -c -o "$tmp/work/$name.o" "$src" 2>"$tmp/$name.err"; then
        cp "$src" "$tmp/work/"
        names="$names $name"
        if [ "$hostlink" = 1 ]; then
            $X -o "$tmp/work/$name.host" "$src" 2>"$tmp/$name.err" \
                || { echo "FAIL $name: kcc could not link it here"; sed 's/^/    /' "$tmp/$name.err"; }
            $X -dynamic -o "$tmp/work/$name.dyn" "$src" 2>"$tmp/$name.err" \
                || { echo "FAIL $name: kcc could not link it here with -dynamic"; sed 's/^/    /' "$tmp/$name.err"; }
        fi
    else
        echo "FAIL $name: kcc failed"
        sed 's/^/    /' "$tmp/$name.err"
    fi
done
cp -r tests/include "$tmp/work/" 2>/dev/null

echo "linking and running in Docker"
docker run --rm --platform linux/amd64 -v "$tmp/work:/w" -e NAMES="$names" kcc-x86_64-linux sh -c '
    cd /w; pass=0; fail=0
    for name in $NAMES; do
        if ! gcc -std=c89 -w -o $name.ref $name.c 2>/dev/null; then echo "SKIP $name: gcc does not accept it"; continue; fi
        ./$name.ref > $name.ref.out 2>&1; want=$?
        if ! gcc -o $name.kcc $name.o 2>$name.ld.err; then echo "FAIL $name: link failed"; sed "s/^/    /" $name.ld.err | head -5; fail=$((fail+1)); continue; fi
        ./$name.kcc > $name.kcc.out 2>&1; got=$?
        if [ $got -ne $want ]; then echo "FAIL $name: exit status $got, gcc gives $want"; fail=$((fail+1))
        elif ! cmp -s $name.ref.out $name.kcc.out; then echo "FAIL $name: output differs"; diff $name.ref.out $name.kcc.out | head -10 | sed "s/^/    /"; fail=$((fail+1))
        else echo "PASS $name (exit $got)"; pass=$((pass+1)); fi
        for how in host dyn; do
            [ -f $name.$how ] || continue
            if [ $how = dyn ]; then what="linked here, dynamic"; else what="linked here"; fi
            ./$name.$how > $name.$how.out 2>&1; got=$?
            if [ $got -ne $want ]; then echo "FAIL $name $what: exit status $got, gcc gives $want"; fail=$((fail+1))
            elif ! cmp -s $name.ref.out $name.$how.out; then echo "FAIL $name $what: output differs"; fail=$((fail+1))
            else echo "PASS $name $what (exit $got)"; pass=$((pass+1)); fi
        done
    done
    echo "$pass passed, $fail failed"; [ $fail -eq 0 ]'
