/* anonymous struct and union members (C11; gcc and clang accept them in C89 mode) */
#include <stdio.h>
#include <stddef.h>
struct S {
    int a;
    union {
        int b1;
        unsigned char b2;
    };
    struct { union { struct { short c; }; }; };
    struct {
        int d;
        struct { long e; } named;
    };
    char f;
};
union U {
    struct { unsigned char x, y; };
    unsigned short xy;
};
struct T { int a; union { int c; int d; }; struct S s; };
struct T gt = { 1, { 2 }, { 3, { 4 }, { { { 5 } } }, { 6, { 7 } }, 8 } };
static int sum(const struct S *p) { return p->a + p->b1 + p->c + p->d + (int)p->named.e + p->f; }
int main(void) {
    struct S v;
    struct S *p = &v;
    union U u;
    v.a = 1; v.b1 = 2; v.c = 3; v.d = 4; v.named.e = 5; v.f = 6;
    printf("%d %d %u %d %d %ld %d\n", v.a, v.b1, v.b2, v.c, v.d, v.named.e, v.f);
    p->c += 10; p->b2 = 9;
    printf("%d %d %d %d\n", v.c, v.b1, sum(p), (int)sizeof(struct S));
    printf("%d %d %d %d %d\n", (int)offsetof(struct S, b1), (int)offsetof(struct S, b2), (int)offsetof(struct S, c),
           (int)offsetof(struct S, d), (int)offsetof(struct S, named.e));
    u.xy = 0x1234; u.x += 1;
    printf("%x %x %x\n", u.x, u.y, u.xy);
    printf("%d %d %d %d %d %d %d %ld %d\n", gt.a, gt.c, gt.d, gt.s.a, gt.s.b1, gt.s.c, gt.s.d, gt.s.named.e, gt.s.f);
    return sum(&gt.s);
}
