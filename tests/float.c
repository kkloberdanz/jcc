/*
 * Step 5 test: floating point. Literals, arithmetic, conversions in both
 * directions including unsigned 64 bit, comparisons, conditions,
 * increments, compound assignment, arguments beyond the register count,
 * variadic doubles, returns, initializers and va_arg.
 */
#include <stdarg.h>
#include <stdio.h>

struct Rec { int n; double avg; float w; };

static double gd = 2.5;
static float gf = -1.25f;
static double from_int = 7;
static float fs[4] = { 1, 2.5, -3, 4e2 };
static struct Rec rec = { 3, 1.5, 0.5f };
static double *pgd = &gd;

static double add(double a, double b) { return a + b; }
static float addf(float a, float b) { return a + b; }
static double mixed(int a, double b, long c, float d, unsigned e) { return a + b + c + d + e; }
static int order(double a, int b, double c, int d) { return (int)(a * 1000 + b * 100 + c * 10 + d); }

/* ten doubles: the last two go on the stack */
static double ten(double a, double b, double c, double d, double e, double f, double g, double h, double i, double j) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j;
}

/* ints and floats interleaved past both register files */
static double many(int i1, double d1, int i2, double d2, int i3, double d3, int i4, double d4, int i5, double d5,
                   int i6, double d6, int i7, double d7, int i8, double d8, int i9, double d9, float f10) {
    return i1 + i2 + i3 + i4 + i5 + i6 + i7 + i8 + i9 + d1 + d2 + d3 + d4 + d5 + d6 + d7 + d8 + d9 + f10;
}

static double vsum(int n, ...) {
    va_list ap;
    double s = 0;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) {
        if (i % 3 == 2) {
            s += va_arg(ap, int);
        } else {
            s += va_arg(ap, double);
        }
    }
    va_end(ap);
    return s;
}

static double apply(double (*f)(double, double), double x, double y) { return f(x, y); }

static int sign(double d) {
    if (d > 0) {
        return 1;
    } else if (d < 0) {
        return -1;
    }
    return 0;
}

int main(void) {
    double d = 1.5;
    float f = 2.25f;
    int i = 7;
    unsigned u = 4000000000u;
    long l = -123456789012L;
    unsigned long ul = 18000000000000000000ul;
    unsigned char uc;
    double arr[3] = { 0.5, 1.5, 2.5 };
    double *p = arr;
    double nan = 0.0 / 0.0;
    double big;
    double t;
    int k;

    printf("%g %g %g %g %g\n", d, f, gd, gf, from_int);
    printf("%g %g %g %g | %d %g %g | %g\n", fs[0], fs[1], fs[2], fs[3], rec.n, rec.avg, rec.w, *pgd);
    printf("%d %d %d\n", (int)sizeof(d), (int)sizeof(f), (int)sizeof(1.0f));

    /* arithmetic and mixed conversions */
    printf("%g %g %g %g\n", d + f, d - i, d * l, i / d);
    printf("%g %g %g\n", u + 0.5, (double)ul, (float)ul);
    printf("%g %g\n", -d, -f);
    printf("%d %ld %u %lu\n", (int)-2.75, (long)1e12, (unsigned)3e9, (unsigned long)1.7e19);
    printf("%d %d\n", (int)(float)16777217, (int)(double)(float)0.1f == 0);
    uc = (unsigned char)200.7;
    printf("%d\n", uc);
    big = 9007199254740993.0;    /* 2^53 + 1, rounds */
    printf("%ld %g\n", (long)big, (double)(long)big - big);

    /* comparisons, including against NaN */
    printf("%d %d %d %d %d %d\n", d < f, d > f, d <= 1.5, d >= 2, d == 1.5, d != 1.5);
    printf("%d %d %d %d %d %d\n", nan < 1, nan > 1, nan <= 1, nan >= 1, nan == nan, nan != nan);
    printf("%d %d %d\n", i > d, 2 == 2.0, f < i);

    /* conditions */
    printf("%d %d %d %d\n", !d, !0.0, d && f, 0.0 || gf);
    printf("%d %d %d\n", sign(-0.5), sign(3), sign(0));
    if (d) { printf("yes\n"); }
    if (!(d - 1.5)) { printf("zero\n"); }
    k = 0;
    while (d < 100) { d *= 2; k++; }
    printf("%d %g %s\n", k, d, d > 50 ? "big" : "small");
    d = 1.5;

    /* increments and compound assignment */
    t = d++;
    printf("%g %g\n", t, d);
    t = ++d;
    printf("%g %g\n", t, d);
    t = d--;
    printf("%g %g\n", t, d);
    t = --d;
    printf("%g %g\n", t, d);
    t = f++;
    printf("%g %g\n", t, f);
    d += 1;
    d *= 3;
    d /= 2;
    d -= f;
    printf("%g\n", d);
    i = 10;
    i += 2.6;
    i /= 2.5;
    printf("%d\n", i);
    f += i;
    f -= 0.25;
    printf("%g\n", f);
    arr[1] += arr[0] * 4;
    *p++ += 1;
    printf("%g %g %g %g\n", arr[0], arr[1], *p, p[1]);
    rec.avg = rec.avg * 2 + rec.w;
    rec.w = rec.avg;
    printf("%g %g\n", rec.avg, rec.w);

    /* calls */
    printf("%g %g %g %d\n", add(1.5, 2.25), addf(1.5f, 2.25f), mixed(1, 2.5, 3, 4.5f, 5), order(1.5, 2, 3.5, 4));
    printf("%g\n", ten(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
    printf("%g\n", many(1, 1.5, 2, 2.5, 3, 3.5, 4, 4.5, 5, 5.5, 6, 6.5, 7, 7.5, 8, 8.5, 9, 9.5, 0.25f));
    printf("%g %g\n", vsum(0), vsum(7, 1.5, 2.5, 3, 4.5, 5.5, 6, 7.5));
    printf("%g %g\n", apply(add, 3, 4), apply(add, f, d));
    printf("%.2f %5.1f %e\n", 3.14159, 2.5f, 12345.678);
    printf("%g %g\n", 1e300 * 1e10, -1e300 * 1e10);
    return (int)(add(20, 22.5) + 0.25);
}
