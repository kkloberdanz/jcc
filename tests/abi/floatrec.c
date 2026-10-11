#include <stdio.h>
#include "floatrec.h"
static void print128(const char *tag, u128 v) {
    printf("%s %lx %lx\n", tag, (unsigned long)(v >> 64), (unsigned long)v);
}
double caller_sum(struct F3 a, struct D2 b, struct IFF c, struct Big d, i128 e) {
    printf("caller: sum %g %g %g | %g %g | %d %g %g | %g %g %g %g %g | %ld\n",
           a.a, a.b, a.c, b.a, b.b, c.a, c.b, c.c, d.a, d.b, d.c, d.d, d.e, (long)e);
    return sum_f3(a) + sum_d2(b) + sum_iff(c) + sum_big(d) + (double)e;
}
struct D4 caller_make_d4(double a) { struct D4 s; s.a = a * 2; s.b = a * 3; s.c = a * 4; s.d = a * 5; return s; }
int main(void) {
    struct F1 f1 = make_f1(1.5);
    struct F3 f3 = make_f3(1, 2, 3);
    struct F4 f4 = make_f4(10);
    struct D2 d2 = make_d2(3.5, 4.5);
    struct D4 d4 = make_d4(20);
    struct FD fd = make_fd(5, 6);
    struct IF if_ = make_if(7, 8.5);
    struct IFF iff = make_iff(9, 10, 11);
    struct DI di = make_di(12.5, 13);
    struct Big big = make_big(100);
    struct CF cf = make_cf('a', 0.25);
    union UD ud = make_ud(14, 15);
    i128 one = 1;
    i128 a = one << 100;
    i128 b = -(one << 70) - 12345;
    printf("%g %g %g %g %g %g\n", f1.a, f3.a + f3.b + f3.c, f4.a[0] + f4.a[3], d2.a + d2.b, d4.a + d4.d, fd.a + fd.b);
    printf("%d %g %d %g %g %g %g %d %g %g %g\n", if_.a, if_.b, iff.a, iff.b + iff.c, di.a, (double)di.b, big.a + big.e, cf.c, cf.f, ud.f[0], ud.f[1]);
    printf("%g %g %g %g %g %g\n", sum_f1(f1), sum_f3(f3), sum_f4(f4), sum_d2(d2), sum_d4(d4), sum_fd(fd));
    printf("%g %g %g %g %g %g\n", sum_if(if_), sum_iff(iff), sum_di(di), sum_big(big), sum_cf(cf), sum_ud(ud));
    printf("spill %g\n", spill(d2, d4, f3, f4, 42, if_, 2.5, f1, -7, make_d2(1, 2), 0.125));
    printf("vsum %g\n", vsum(2, f3, d2, if_, di, big, a, 1, 2.5, make_f3(3, 4, 5), make_d2(6, 7), make_if(8, 9), make_di(10, 11), make_big(12), b, 13, 14.5));
    print128("add", (u128)add_i128(a, b));
    print128("mul", mul_u128((u128)a, 3));
    print128("mul2", mul_u128(0xffffffffffffffffull, 0xffffffffffffffffull));
    print128("many", (u128)many_i128(1, a, 2, b, 3, -a, one, 4, b));
    print128("vsum", (u128)vsum_i128(4, a, b, one, -one));
    printf("%.0f %.0f\n", to_double(a), to_double(b));
    print128("from", (u128)from_double(-1e25));
    return (int)sum_f3(f3) + (int)(add_i128(a, b) >> 90);
}
