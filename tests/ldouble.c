/* long double: a double on Apple arm64, the x87 80 bit format on x86_64 Linux */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

struct LD { long double x; int k; };
struct Mixed { int a; long double b; double c; };

static long double gl = 2.5L;
static long double garr[3] = { 1, -0.125L, 1e10L };
static struct LD gs = { 3.75L, 7 };
static long double zero = 0.0L;

static long double add(long double a, long double b) { return a + b; }
static long double sub(long double a, long double b) { return a - b; }
static long double mul(long double a, long double b) { return a * b; }
static long double dvd(long double a, long double b) { return a / b; }

static struct LD make(long double x, int k) {
    struct LD r;
    r.x = x;
    r.k = k;
    return r;
}

static long double sum_struct(struct LD s, struct Mixed m) {
    return s.x + s.k + m.a + m.b + m.c;
}

static long double vsum(int n, ...) {
    va_list ap;
    long double s = 0;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) {
        s += va_arg(ap, long double);
    }
    va_end(ap);
    return s;
}

/* seven doubles then stack arguments, with long doubles in between: every one is in memory */
static long double mixed_args(double d0, long double l0, double d1, long double l1, int i0,
                              double d2, double d3, double d4, double d5, double d6, double d7,
                              long double l2, double d8, int i1) {
    return d0 + l0 + d1 + l1 + i0 + d2 + d3 + d4 + d5 + d6 + d7 + l2 + d8 + i1;
}

static int conversions(void) {
    long double x = -2.75L;
    long double big = 9223372036854775808.0L;      /* 2^63 */
    unsigned long long u = 18446744073709551615ULL;
    long long ll = -1234567890123LL;
    unsigned int ui = 4000000000u;
    short sh = -300;
    unsigned char uc = 200;
    float f = 1.5f;
    double d = -0.0625;
    printf("to int %d %u %ld %lu %lld %hd %d\n", (int)x, (unsigned)-x, (long)x, (unsigned long)-x,
           (long long)x, (short)x, (int)(unsigned char)-x);
    printf("to float %g %g\n", (double)(float)x, (double)x);
    printf("from int %Lg %Lg %Lg %Lg %Lg\n", (long double)ll, (long double)ui, (long double)sh,
           (long double)uc, (long double)-7);
    printf("from float %Lg %Lg\n", (long double)f, (long double)d);
    printf("unsigned 64 %.0Lf %.0Lf %llu %llu\n", (long double)u, big * 1.5L,
           (unsigned long long)(big * 1.5L), (unsigned long long)(big - 1024.0L));
    printf("truncation %lld %lld %d\n", (long long)(x * 1000), (long long)-x, (int)(long double)0.999L);
    return 0;
}

static int comparisons(void) {
    long double a = 1.5L;
    long double b = 2.25L;
    long double nan = zero / zero;
    printf("cmp %d %d %d %d %d %d\n", a < b, a > b, a <= b, a >= b, a == b, a != b);
    printf("cmp %d %d %d %d %d %d\n", b < a, b > a, b <= a, b >= a, a == a, a != a);
    printf("nan %d %d %d %d %d %d %d\n", nan < a, nan > a, nan <= a, nan >= a, nan == a, nan != a, nan != nan);
    printf("truth %d %d %d %d\n", !a, !zero, a && b, zero || b);
    if (nan) {
        printf("nan is true\n");
    }
    if (!zero) {
        printf("zero is false\n");
    }
    printf("mixed %d %d %d\n", a < 2, 2.0 > a, 1.5f == a);
    return 0;
}

static int arithmetic(void) {
    long double x = 10.5L;
    long double y = 4;
    long double z;
    int i;
    printf("ops %Lg %Lg %Lg %Lg %Lg\n", x + y, x - y, x * y, x / y, -x);
    printf("calls %Lg %Lg %Lg %Lg\n", add(x, y), sub(x, y), mul(x, y), dvd(x, y));
    z = x;
    z += 1; z -= 0.5L; z *= 2; z /= 4;
    printf("compound %Lg\n", z);
    printf("incdec %Lg", z++);
    printf(" %Lg", ++z);
    printf(" %Lg", z--);
    printf(" %Lg\n", --z);
    printf("ternary %Lg %Lg\n", x > y ? x : y, x < y ? x : y);
    printf("comma %Lg\n", (x = 3, y = x * 2, x + y));
    printf("mixed %Lg %g %Lg\n", x + 1, (double)(x * 0.5), 2 * x / 1.0f);
    /* stack discipline: dropped values, in a loop long enough to overflow the x87 stack if they leak */
    for (z = 0; z < 20; z += 0.5L) {
        x;
        (void)y;
        add(z, 1);
        x = z;
        x = y = z;
        i = (int)(z * 2);
    }
    printf("loop %Lg %d\n", z, i);
    for (i = 0, z = 1; i < 60; i++) {
        z = z * 1.5L - 0.25L;
        if (z > 1000) {
            z = z / 1024;
        }
        z = (z > 2) ? z - 1 : z + (long double)i;
    }
    printf("iter %.6Lf\n", z);
    return 0;
}

int main(void) {
    long double a = 1.5L;
    long double arr[2] = { 2, 0.25L };
    struct LD s = make(5.5L, 2);
    struct Mixed m;
    char buf[64];
    m.a = 1;
    m.b = 0.5L;
    m.c = 0.25;
    printf("statics %Lg %Lg %Lg %Lg %Lg %d\n", gl, garr[0], garr[1], garr[2], gs.x, gs.k);
    printf("autos %Lg %Lg %Lg\n", a, arr[0], arr[1]);
    printf("struct %Lg %d %Lg\n", s.x, s.k, sum_struct(s, m));
    printf("variadic %Lg %.3Lf %Le\n", vsum(3, 1.5L, 2.25L, -1.0L), a, a);
    sprintf(buf, "%Lf|%Lg|%08.2Lf", a, gl, -a);
    printf("sprintf %s\n", buf);
    printf("mixed args %Lg\n", mixed_args(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14));
    printf("sizeof %d\n", (int)sizeof(long double) >= 8);
    conversions();
    comparisons();
    arithmetic();
    return 0;
}
