/* jcc: syntax-only. GNU: a cast of a struct or union to its own type is the operand as a value (clang rejects it, so this is not differential; c-testsuite 00216 checks the result) */
struct S { int a, b; };
union U { int i; float f; };
typedef struct S S2;

struct S copy(const struct S *p) {
    struct S s = (struct S)*p;
    union U u = { 1 };
    union U v = (union U)u;
    struct { struct S s; int c; } w = { (struct S)s, (int)v.i };
    return (S2)w.s;
}
