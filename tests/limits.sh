#!/bin/sh
# The C89 translation limits (ANSI X3.159-1989 2.2.4.1, ISO 9899:1990 5.2.4.1):
# a conforming compiler must translate and execute at least one program
# that contains one instance of every minimum limit at once. This script
# generates that program (and its 8 nested headers), builds it with the
# host compiler and with jcc, and compares the output. Run from the
# repository root (make test does).
cd "$(dirname "$0")/.." || exit 1
JCC=${JCC:-./jcc}
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
# 8 nesting levels for #included files: the innermost defines INC_DEPTH
i=1
while [ $i -le 8 ]; do
    if [ $i -lt 8 ]; then echo "#include \"inc$((i + 1)).h\"" >"$tmp/inc$i.h"
    else echo "#define INC_DEPTH 8" >"$tmp/inc$i.h"; fi
    i=$((i + 1))
done
awk 'BEGIN {
    print "#include <stdio.h>"
    print "#include <string.h>"
    print "#include \"inc1.h\""
    # 8 nesting levels of conditional inclusion
    for (i = 1; i <= 8; i++) print "#if INC_DEPTH == 8"
    print "#define COND_DEPTH 8"
    for (i = 1; i <= 8; i++) print "#endif"
    # 1024 macro identifiers simultaneously defined
    for (i = 1; i <= 1024; i++) print "#define MACRO_" i " " i
    # 31 significant initial characters in a macro name
    print "#define m123456789012345678901234567890 1"
    print "#define m123456789012345678901234567891 2"
    # 31 parameters in a macro definition, 31 arguments in a macro invocation
    s = "#define MAC31("; for (i = 1; i <= 31; i++) s = s (i > 1 ? "," : "") "a" i
    s = s ") ("; for (i = 1; i <= 31; i++) s = s (i > 1 ? "+" : "") "a" i
    print s ")"
    # 511 external identifiers in one translation unit (2 of them differ only in the 6th character)
    print "int extab1 = 1;"
    print "int extab2 = 2;"
    for (i = 1; i <= 509; i++) print "int ext_" i " = " i ";"
    # 127 members in a structure, 127 enumeration constants
    print "struct s127 {"; for (i = 1; i <= 127; i++) print "    int m" i ";"; print "};"
    print "enum e127 {"; for (i = 1; i <= 127; i++) print "    E" i " = " i ","; print "    E_END };"
    # 15 levels of nested structure definitions
    s = "struct n1 { "; for (i = 2; i <= 15; i++) s = s "struct n" i " { "
    s = s "int v; "; for (i = 15; i >= 2; i--) s = s "} s" i "; "
    print s "} nest;"
    # 32767 bytes in an object
    print "static char big[32767];"
    # 12 pointer declarators modifying one type
    print "static int ************p12;"
    # 509 characters in a string literal (after concatenation) and in a logical source line
    s = "static const char lit509[] = \""; for (i = 1; i <= 254; i++) s = s "a"; print s "\""
    s = "    \""; for (i = 1; i <= 255; i++) s = s "b"; print s "\";"
    s = "static const char line509[] = \""; while (length(s) < 507) s = s "c"; print s "\";"
    # 31 parameters in a function definition
    s = "static int f31("; for (i = 1; i <= 31; i++) s = s (i > 1 ? ", " : "") "int p" i
    print s ") {"
    s = "    return "; for (i = 1; i <= 31; i++) s = s (i > 1 ? " + " : "") "p" i; print s ";"
    print "}"
    # 257 case labels in one switch statement
    print "static int sw(int k) {"
    print "    switch (k) {"
    for (i = 0; i <= 256; i++) print "        case " i ": return " i " * 2 + 1;"
    print "    }"
    print "    return -1;"
    print "}"
    print "int main(void) {"
    print "    struct s127 s;"
    print "    int sum = 0, i;"
    print "    int *p1, **p2, ***p3, ****p4, *****p5, ******p6, *******p7, ********p8, *********p9, **********p10, ***********p11;"
    # 31 significant initial characters in an internal identifier
    print "    int a123456789012345678901234567890 = 1;"
    print "    int a123456789012345678901234567891 = 2;"
    # 31 declarators nested by parentheses
    s = "    int "; for (i = 1; i <= 31; i++) s = s "("; s = s "d31"; for (i = 1; i <= 31; i++) s = s ")"; print s " = 31;"
    # 32 expressions nested by parentheses
    s = "    int e32 = "; for (i = 1; i <= 32; i++) s = s "("; s = s "32"; for (i = 1; i <= 32; i++) s = s ")"; print s ";"
    print "    {"
    # 127 identifiers with block scope in one block
    for (i = 1; i <= 127; i++) print "        int b" i " = " i ";"
    for (i = 1; i <= 127; i++) print "        sum += b" i ";"
    print "        printf(\"block %d\\n\", sum);"
    print "    }"
    # 15 nesting levels of compound, iteration and selection statements
    print "    sum = 0;"
    for (i = 1; i <= 15; i++) print "    if (i == 0 || 1) {"
    print "        sum = 15;"
    for (i = 1; i <= 15; i++) print "    }"
    print "    printf(\"nesting %d %d %d\\n\", sum, COND_DEPTH, INC_DEPTH);"
    print "    sum = 0;"
    for (i = 1; i <= 509; i++) print "    sum += ext_" i ";"
    print "    printf(\"externals %d %d %d\\n\", sum, extab1, extab2);"
    print "    printf(\"macros %d %d %d %d\\n\", MACRO_1 + MACRO_512 + MACRO_1024, m123456789012345678901234567890, m123456789012345678901234567891,"
    s = "        MAC31("; for (i = 1; i <= 31; i++) s = s (i > 1 ? "," : "") i; print s "));"
    print "    printf(\"identifiers %d %d %d %d\\n\", a123456789012345678901234567890, a123456789012345678901234567891, d31, e32);"
    print "    memset(&s, 0, sizeof s); s.m1 = 1; s.m127 = 127;"
    print "    printf(\"struct %d %d %d\\n\", (int)sizeof s, s.m1 + s.m127, (int)sizeof(struct s127) / (int)sizeof(int));"
    print "    printf(\"enum %d %d\\n\", E127, E_END);"
    print "    nest.s2.s3.s4.s5.s6.s7.s8.s9.s10.s11.s12.s13.s14.s15.v = 15;"
    print "    printf(\"nested %d\\n\", nest.s2.s3.s4.s5.s6.s7.s8.s9.s10.s11.s12.s13.s14.s15.v);"
    print "    big[0] = 1; big[32766] = 2;"
    print "    printf(\"object %d %d\\n\", (int)sizeof big, big[0] + big[32766]);"
    print "    i = 12; p1 = &i; p2 = &p1; p3 = &p2; p4 = &p3; p5 = &p4; p6 = &p5; p7 = &p6; p8 = &p7; p9 = &p8; p10 = &p9; p11 = &p10; p12 = &p11;"
    print "    printf(\"pointers %d\\n\", ************p12);"
    print "    printf(\"strings %d %d\\n\", (int)strlen(lit509), (int)strlen(line509) + 32);"
    s = "    printf(\"call %d\\n\", f31("; for (i = 1; i <= 31; i++) s = s (i > 1 ? ", " : "") i; print s "));"
    print "    printf(\"switch %d %d %d %d\\n\", sw(0), sw(128), sw(256), sw(257));"
    print "    return 0;"
    print "}"
}' >"$tmp/limits.c"
# the longest line must be exactly 509 characters: the limit, not past it
longest=$(awk '{ if (length($0) > n) n = length($0) } END { print n }' "$tmp/limits.c")
[ "$longest" -eq 509 ] || { echo "LIMITS: generator bug, longest line is $longest"; exit 1; }
if ! cc -std=c89 -w -o "$tmp/ref" "$tmp/limits.c" 2>"$tmp/cc.err"; then
    echo "LIMITS: cc does not accept the program"; cat "$tmp/cc.err"; exit 1
fi
"$tmp/ref" >"$tmp/ref.out" 2>&1
if ! "$JCC" -o "$tmp/jcc" "$tmp/limits.c" 2>"$tmp/jcc.err"; then
    echo "LIMITS FAIL: jcc rejected the program"; sed 's/^/    /' "$tmp/jcc.err"; exit 1
fi
"$tmp/jcc" >"$tmp/jcc.out" 2>&1
if cmp -s "$tmp/ref.out" "$tmp/jcc.out"; then
    echo "LIMITS OK ($(wc -l <"$tmp/limits.c" | tr -d ' ') lines)"
else
    echo "LIMITS FAIL: output differs from cc's"; diff "$tmp/ref.out" "$tmp/jcc.out" | sed 's/^/    /'; exit 1
fi
