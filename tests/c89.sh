#!/bin/sh
# The C89 constraints: every "Constraints" paragraph of section 3 of ANSI
# X3.159-1989 (ISO 9899:1990 numbers the same clauses 6.x) names something a
# program shall not do, and a conforming compiler must diagnose it. Each
# tests/c89/*.c violates one constraint, cited on its first line, on the
# line marked `/* violation */`; kcc must reject the file with a diagnostic
# on that line. The host compiler must reject the file too, as strict C89
# (-std=c89 -pedantic-errors, so gcc and clang agree), which guards the
# tests themselves: one it accepts is reported INVALID. Constraints that
# the gnu89 dialect kcc speaks deliberately does not enforce are not here
# (duplicate qualifiers, non-constant initializers for automatic
# aggregates, excess initializers, macro redefinition, enumerators beyond
# int, sizeof void). Tests run JOBS at a time (default: the number of
# processors).
# Run from the repository root (make test does).
cd "$(dirname "$0")/.." || exit 1
KCC=${KCC:-./kcc}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/res"

# one test: a line of report in res/NAME when it fails, nothing when it passes
cat >"$tmp/one.sh" <<'EOF'
src=$1; name=$(basename "$src" .c); out="$tmp/res/$name"
line=$(grep -n "violation \*/" "$src" | head -1 | cut -d: -f1)
if [ -z "$line" ]; then
    echo "INVALID $name: no line is marked" >"$out"
elif cc -std=c89 -pedantic-errors -fsyntax-only "$src" >/dev/null 2>&1; then
    echo "INVALID $name: cc accepts it" >"$out"
elif "$KCC" -fsyntax-only "$src" >"$tmp/$name.err" 2>&1; then
    echo "FAIL $name: accepted" >"$out"
elif ! grep -q ":$line:[0-9]*: " "$tmp/$name.err"; then
    echo "FAIL $name: diagnosed the wrong line, want $line: $(grep -m1 error "$tmp/$name.err" | cut -c1-110)" >"$out"
else
    : >"$out"
fi
EOF
export tmp KCC
ls tests/c89/*.c | xargs -P "$JOBS" -n 1 sh "$tmp/one.sh"
pass=0; fail=0
for out in "$tmp"/res/*; do
    if [ -s "$out" ]; then cat "$out"; fail=$((fail + 1)); else pass=$((pass + 1)); fi
done
echo "C89 constraints: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
