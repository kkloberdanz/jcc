#include <stdio.h>
#include "record.h"
struct S8 make8(int a, int b) { struct S8 s; s.a = a; s.b = b; return s; }
struct S16 make16(long a, long b) { struct S16 s; s.a = a; s.b = b; return s; }
struct S24 make24(long a, long b, long c) { struct S24 s; s.a = a; s.b = b; s.c = c; return s; }
struct Sc makec(char a, char b, char c) { struct Sc s; s.a = a; s.b = b; s.c = c; return s; }
union U makeu(long l) { union U u; u.l = l; return u; }
long sum8(struct S8 s) { return s.a + s.b; }
long sum12(struct S12 s) { return s.a + s.b + s.c + s.d; }
long sum16(struct S16 s) { return s.a + s.b; }
long sum24(struct S24 s) { return s.a + s.b + s.c; }
long sumc(struct Sc s) { return s.a + s.b + s.c; }
long sumu(union U u) { return u.l + u.c[0]; }
long mixed(int x, struct S24 a, struct S8 b, long y, struct S16 c, char z) {
    struct S12 m = caller_make12(x, (short)y, z, b.a);
    printf("callee: mixed %d %ld %ld %ld %d %d %ld %ld %ld %d  m=%d %d %d %d\n",
           x, a.a, a.b, a.c, b.a, b.b, y, c.a, c.b, z, m.a, m.b, m.c, m.d);
    return x + a.a + a.b + a.c + b.a + b.b + y + c.a + c.b + z
         + caller_sum(b, c, a) + sum12(m);
}
struct S24 pass_through(struct S24 s, int bump) { s.a += bump; s.b += bump; s.c += bump; return s; }
