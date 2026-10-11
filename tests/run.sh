#!/bin/sh
# Differential test: every tests/*.c is built with the system compiler and
# with kcc; the two binaries must exit with the same status and print the
# same output. Tests run JOBS at a time (default: the number of
# processors). Run from the repository root (make test does).
cd "$(dirname "$0")/.." || exit 1
KCC=${KCC:-./kcc}   # the compiler under test; set KCC to test a self-hosted build
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/res"

# one test: its report goes to res/NAME, its verdict (pass, fail or skip) to res/NAME.status
cat >"$tmp/one.sh" <<'EOF'
src=$1; name=$(basename "$src" .c); out="$tmp/res/$name"
verdict() { echo "$1" >"$out.status"; }
# a test marked `kcc: syntax-only` only has to pass semantic analysis
if head -5 "$src" | grep -q "kcc: syntax-only"; then
    if "$KCC" -fsyntax-only "$src" 2>"$tmp/$name.kcc.err"; then
        echo "PASS $name (syntax only)" >"$out"; verdict pass
    else
        { echo "FAIL $name: kcc rejected it"; sed "s/^/    /" "$tmp/$name.kcc.err"; } >"$out"; verdict fail
    fi
    exit 0
fi
if ! cc -std=c89 -w -o "$tmp/$name.ref" "$src" 2>"$tmp/$name.cc.err"; then
    echo "SKIP $name: cc does not accept it" >"$out"; verdict skip
    exit 0
fi
"$tmp/$name.ref" >"$tmp/$name.ref.out" 2>&1
want=$?
if ! "$KCC" -o "$tmp/$name.kcc" "$src" 2>"$tmp/$name.kcc.err"; then
    { echo "FAIL $name: kcc failed"; sed 's/^/    /' "$tmp/$name.kcc.err"; } >"$out"; verdict fail
    exit 0
fi
"$tmp/$name.kcc" >"$tmp/$name.kcc.out" 2>&1
got=$?
if [ "$got" -ne "$want" ]; then
    echo "FAIL $name: exit status $got, cc gives $want" >"$out"; verdict fail
elif ! cmp -s "$tmp/$name.ref.out" "$tmp/$name.kcc.out"; then
    { echo "FAIL $name: output differs from cc's"; diff "$tmp/$name.ref.out" "$tmp/$name.kcc.out" | head -20 | sed 's/^/    /'; } >"$out"; verdict fail
else
    echo "PASS $name (exit $got)" >"$out"; verdict pass
fi
EOF
export tmp KCC
ls tests/*.c | xargs -P "$JOBS" -n 1 sh "$tmp/one.sh"
pass=0
fail=0
for status in "$tmp"/res/*.status; do
    cat "${status%.status}"
    case $(cat "$status") in
        pass) pass=$((pass + 1)) ;;
        fail) fail=$((fail + 1)) ;;
    esac
done
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
