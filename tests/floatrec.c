/* records with floating point members by value: HFAs, mixed records, varargs, returns */
#include <stdarg.h>
#include <stdio.h>

struct F1 { float a; };
struct F2 { float a, b; };
struct F3 { float a, b, c; };
struct F4 { float a[4]; };
struct D1 { double a; };
struct D2 { double a, b; };
struct D3 { double a, b, c; };
struct D4 { double a, b, c, d; };
struct FD { float a; double b; };
struct FI { float a; int b; };
struct IF { int a; float b; };
struct FFI { float a, b; int c; };
struct IFF { int a; float b, c; };
struct DI { double a; int b; };
struct ID { int a; double b; };
struct Big { double a, b, c, d, e; };
struct Nest { struct F2 p; struct F2 q; };
struct CF { char c; float f; };
union UF { float f; unsigned u; };
union UD { double d; float f[2]; };

static struct F1 mkf1(float a) { struct F1 s; s.a = a; return s; }
static struct F2 mkf2(float a, float b) { struct F2 s; s.a = a; s.b = b; return s; }
static struct F3 mkf3(float a, float b, float c) { struct F3 s; s.a = a; s.b = b; s.c = c; return s; }
static struct F4 mkf4(float a) { struct F4 s; int i; for (i = 0; i < 4; i++) s.a[i] = a + i; return s; }
static struct D1 mkd1(double a) { struct D1 s; s.a = a; return s; }
static struct D2 mkd2(double a, double b) { struct D2 s; s.a = a; s.b = b; return s; }
static struct D3 mkd3(double a) { struct D3 s; s.a = a; s.b = a * 2; s.c = a * 3; return s; }
static struct D4 mkd4(double a) { struct D4 s; s.a = a; s.b = a + 1; s.c = a + 2; s.d = a + 3; return s; }
static struct FD mkfd(float a, double b) { struct FD s; s.a = a; s.b = b; return s; }
static struct FI mkfi(float a, int b) { struct FI s; s.a = a; s.b = b; return s; }
static struct IF mkif(int a, float b) { struct IF s; s.a = a; s.b = b; return s; }
static struct FFI mkffi(float a, float b, int c) { struct FFI s; s.a = a; s.b = b; s.c = c; return s; }
static struct IFF mkiff(int a, float b, float c) { struct IFF s; s.a = a; s.b = b; s.c = c; return s; }
static struct DI mkdi(double a, int b) { struct DI s; s.a = a; s.b = b; return s; }
static struct ID mkid(int a, double b) { struct ID s; s.a = a; s.b = b; return s; }
static struct Big mkbig(double a) { struct Big s; s.a = a; s.b = a + 1; s.c = a + 2; s.d = a + 3; s.e = a + 4; return s; }
static struct Nest mknest(float a) { struct Nest s; s.p.a = a; s.p.b = a + 1; s.q.a = a + 2; s.q.b = a + 3; return s; }
static struct CF mkcf(char c, float f) { struct CF s; s.c = c; s.f = f; return s; }
static union UF mkuf(float f) { union UF u; u.f = f; return u; }
static union UD mkud(float a, float b) { union UD u; u.f[0] = a; u.f[1] = b; return u; }

static double sf1(struct F1 s) { return s.a; }
static double sf2(struct F2 s) { return s.a + 10 * s.b; }
static double sf3(struct F3 s) { return s.a + 10 * s.b + 100 * s.c; }
static double sf4(struct F4 s) { return s.a[0] + 10 * s.a[1] + 100 * s.a[2] + 1000 * s.a[3]; }
static double sd1(struct D1 s) { return s.a; }
static double sd2(struct D2 s) { return s.a + 10 * s.b; }
static double sd3(struct D3 s) { return s.a + 10 * s.b + 100 * s.c; }
static double sd4(struct D4 s) { return s.a + 10 * s.b + 100 * s.c + 1000 * s.d; }
static double sfd(struct FD s) { return s.a + 10 * s.b; }
static double sfi(struct FI s) { return s.a + 10 * s.b; }
static double sif(struct IF s) { return s.a + 10 * s.b; }
static double sffi(struct FFI s) { return s.a + 10 * s.b + 100 * s.c; }
static double siff(struct IFF s) { return s.a + 10 * s.b + 100 * s.c; }
static double sdi(struct DI s) { return s.a + 10 * s.b; }
static double sid(struct ID s) { return s.a + 10 * s.b; }
static double sbig(struct Big s) { return s.a + s.b + s.c + s.d + s.e; }
static double snest(struct Nest s) { return s.p.a + 10 * s.p.b + 100 * s.q.a + 1000 * s.q.b; }
static double scf(struct CF s) { return s.c + s.f; }
static double suf(union UF u) { return u.f; }
static double sud(union UD u) { return u.f[0] + 10 * u.f[1]; }

/* registers run out: ten doubles' worth of HFAs, then integers and mixed records */
static double spill(struct D2 a, struct D4 b, struct D3 c, struct F4 d, int i, struct FI e, double x, struct D1 f, long l, struct F2 g) {
    return sd2(a) + sd4(b) + sd3(c) + sf4(d) + i + sfi(e) + x + sd1(f) + l + sf2(g);
}
static double mixed(int i, struct FD a, float f, struct IFF b, double d, struct Big c, struct F3 e, char ch) {
    return i + sfd(a) + f + siff(b) + d + sbig(c) + sf3(e) + ch;
}

static double vsum(int n, ...) {
    va_list ap;
    double s = 0;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) {
        struct F2 p = va_arg(ap, struct F2);
        struct D2 q = va_arg(ap, struct D2);
        struct FI r = va_arg(ap, struct FI);
        struct ID t = va_arg(ap, struct ID);
        struct Big u = va_arg(ap, struct Big);
        int k = va_arg(ap, int);        /* each in its own statement: the order within one expression is unspecified */
        double d = va_arg(ap, double);
        s += sf2(p) + sd2(q) + sfi(r) + sid(t) + sbig(u) + k + d;
    }
    va_end(ap);
    return s;
}

static struct D2 (*fp)(double, double) = mkd2;

int main(void) {
    struct F2 f2 = mkf2(1, 2);
    struct D2 d2 = mkd2(3.5, 4.5);
    struct FI fi = mkfi(5.25, 6);
    struct ID id = mkid(7, 8.125);
    struct Big big = mkbig(100);
    struct D2 viafp = fp(0.5, 0.25);
    struct Nest nest = mknest(1);
    printf("%g %g %g %g\n", sf1(mkf1(1.5)), sf2(f2), sf3(mkf3(1, 2, 3)), sf4(mkf4(1)));
    printf("%g %g %g %g\n", sd1(mkd1(1.5)), sd2(d2), sd3(mkd3(1)), sd4(mkd4(1)));
    printf("%g %g %g %g %g\n", sfd(mkfd(1, 2)), sfi(fi), sif(mkif(1, 2)), sffi(mkffi(1, 2, 3)), siff(mkiff(1, 2, 3)));
    printf("%g %g %g %g %g\n", sdi(mkdi(1, 2)), sid(id), sbig(big), snest(nest), scf(mkcf('a', 0.5)));
    printf("%g %g %g\n", suf(mkuf(2.5)), sud(mkud(1, 2)), sd2(viafp));
    printf("%g\n", spill(d2, mkd4(1), mkd3(2), mkf4(3), 4, fi, 5.5, mkd1(6), 7, f2));
    printf("%g\n", mixed(1, mkfd(2, 3), 4, mkiff(5, 6, 7), 8, big, mkf3(9, 10, 11), 'b'));
    printf("%g\n", vsum(2, f2, d2, fi, id, big, 1, 2.5, mkf2(3, 4), mkd2(5, 6), mkfi(7, 8), mkid(9, 10), mkbig(11), 12, 13.5));
    nest.q = f2;
    f2 = nest.p;
    printf("%g %g %g\n", f2.a, nest.q.b, mkf4(2).a[3]);
    return (int)sf2(f2) + (int)sd2(d2);
}
