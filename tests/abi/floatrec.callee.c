#include <stdarg.h>
#include <stdio.h>
#include "floatrec.h"
struct F1 make_f1(float a) { struct F1 s; s.a = a; return s; }
struct F3 make_f3(float a, float b, float c) { struct F3 s; s.a = a; s.b = b; s.c = c; return s; }
struct F4 make_f4(float a) { struct F4 s; int i; for (i = 0; i < 4; i++) s.a[i] = a + i; return s; }
struct D2 make_d2(double a, double b) { struct D2 s; s.a = a; s.b = b; return s; }
struct D4 make_d4(double a) { struct D4 s; s.a = a; s.b = a + 1; s.c = a + 2; s.d = a + 3; return s; }
struct FD make_fd(float a, double b) { struct FD s; s.a = a; s.b = b; return s; }
struct IF make_if(int a, float b) { struct IF s; s.a = a; s.b = b; return s; }
struct IFF make_iff(int a, float b, float c) { struct IFF s; s.a = a; s.b = b; s.c = c; return s; }
struct DI make_di(double a, int b) { struct DI s; s.a = a; s.b = b; return s; }
struct Big make_big(double a) { struct Big s; s.a = a; s.b = a + 1; s.c = a + 2; s.d = a + 3; s.e = a + 4; return s; }
struct CF make_cf(char c, float f) { struct CF s; s.c = c; s.f = f; return s; }
union UD make_ud(float a, float b) { union UD u; u.f[0] = a; u.f[1] = b; return u; }
double sum_f1(struct F1 s) { return s.a; }
double sum_f3(struct F3 s) { return s.a + 10 * s.b + 100 * s.c; }
double sum_f4(struct F4 s) { return s.a[0] + 10 * s.a[1] + 100 * s.a[2] + 1000 * s.a[3]; }
double sum_d2(struct D2 s) { return s.a + 10 * s.b; }
double sum_d4(struct D4 s) { return s.a + 10 * s.b + 100 * s.c + 1000 * s.d; }
double sum_fd(struct FD s) { return s.a + 10 * s.b; }
double sum_if(struct IF s) { return s.a + 10 * s.b; }
double sum_iff(struct IFF s) { return s.a + 10 * s.b + 100 * s.c; }
double sum_di(struct DI s) { return s.a + 10 * s.b; }
double sum_big(struct Big s) { return s.a + s.b + s.c + s.d + s.e; }
double sum_cf(struct CF s) { return s.c + s.f; }
double sum_ud(union UD u) { return u.f[0] + 10 * u.f[1]; }
double spill(struct D2 a, struct D4 b, struct F3 c, struct F4 d, int i, struct IF e, double x, struct F1 f, long l, struct D2 g, float h) {
    struct D4 back = caller_make_d4(x);
    printf("callee: spill %g %g | %g %g %g %g | %g %g %g | %g %g %g %g | %d | %d %g | %g | %g | %ld | %g %g | %g | back %g %g %g %g\n",
           a.a, a.b, b.a, b.b, b.c, b.d, c.a, c.b, c.c, d.a[0], d.a[1], d.a[2], d.a[3], i, e.a, e.b, x, f.a, l, g.a, g.b, h,
           back.a, back.b, back.c, back.d);
    return sum_d2(a) + sum_d4(b) + sum_f3(c) + sum_f4(d) + i + sum_if(e) + x + sum_f1(f) + l + sum_d2(g) + h + sum_d4(back);
}
double vsum(int n, ...) {
    va_list ap;
    double s = 0;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) {
        struct F3 p = va_arg(ap, struct F3);
        struct D2 q = va_arg(ap, struct D2);
        struct IF r = va_arg(ap, struct IF);
        struct DI t = va_arg(ap, struct DI);
        struct Big u = va_arg(ap, struct Big);
        i128 w = va_arg(ap, i128);
        int k = va_arg(ap, int);
        double d = va_arg(ap, double);
        s += sum_f3(p) + sum_d2(q) + sum_if(r) + sum_di(t) + sum_big(u) + (double)w + k + d;
    }
    va_end(ap);
    return s;
}
i128 add_i128(i128 a, i128 b) { return a + b; }
u128 mul_u128(u128 a, u128 b) { return a * b; }
i128 many_i128(int a, i128 b, int c, i128 d, int e, i128 f, i128 g, int h, i128 i) {
    struct F3 f3 = make_f3(1, 2, 3);
    struct D2 d2 = make_d2(4, 5);
    struct IFF iff = make_iff(6, 7, 8);
    return a + b + c + d + e + f + g + h + i + (i128)caller_sum(f3, d2, iff, make_big(9), b - d);
}
i128 vsum_i128(int n, ...) {
    va_list ap;
    i128 s = 0;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) {
        s = s * 3 + va_arg(ap, i128);
    }
    va_end(ap);
    return s;
}
double to_double(i128 a) { return (double)a; }
i128 from_double(double d) { return (i128)d; }
