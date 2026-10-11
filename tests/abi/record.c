#include <stdio.h>
#include "record.h"
long caller_sum(struct S8 a, struct S16 b, struct S24 c) {
    printf("caller: sum %d %d %ld %ld %ld %ld %ld\n", a.a, a.b, b.a, b.b, c.a, c.b, c.c);
    return a.a + a.b + b.a + b.b + c.a + c.b + c.c;
}
struct S12 caller_make12(int a, short b, char c, int d) { struct S12 s; s.a = a; s.b = b; s.c = c; s.d = d; return s; }
int main(void) {
    struct S8 s8 = make8(1, -2);
    struct S16 s16 = make16(300000000000L, -5);
    struct S24 s24 = make24(7, 8, -900000000000L);
    struct Sc sc = makec('a', -1, 100);
    union U u = makeu(0x0102030405060708L);
    struct S12 s12; s12.a = 1; s12.b = -2; s12.c = 3; s12.d = 4;
    printf("%d %d | %ld %ld | %ld %ld %ld | %d %d %d | %lx\n",
           s8.a, s8.b, s16.a, s16.b, s24.a, s24.b, s24.c, sc.a, sc.b, sc.c, u.l);
    printf("%ld %ld %ld %ld %ld %ld\n", sum8(s8), sum12(s12), sum16(s16), sum24(s24), sumc(sc), sumu(u));
    printf("mixed = %ld\n", mixed(-7, s24, s8, 1000, s16, 'z'));
    s24 = pass_through(pass_through(s24, 1), 2);
    printf("%ld %ld %ld\n", s24.a, s24.b, s24.c);
    return (int)(sum8(make8(40, 2)));
}
