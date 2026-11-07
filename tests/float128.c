/* _Float128 (x86_64): arithmetic, comparisons, conversions with every arithmetic type, literals
   (f128 and q), static data, arguments in registers and on the stack, and glibc's *f128 functions */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>

static _Float128 gq = 1.25f128;
static _Float128 table[3] = { 1.5q, -2.0, 1e100 };

static _Float128 mul_add(_Float128 a, _Float128 b, _Float128 c) { return a * b + c; }
static _Float128 many(double d0, _Float128 a, int i, _Float128 b, _Float128 c, _Float128 d, _Float128 e,
                      _Float128 f, _Float128 g, _Float128 h, _Float128 k) {
    return d0 + a + i + b + c + d + e + f + g + h + k;
}

static void show(const char *what, _Float128 v) {
    char buf[64];
    strfromf128(buf, sizeof buf, "%.33g", v);
    printf("%s %s\n", what, buf);
}

int main(void) {
    _Float128 x = 1, y = 3, z;
    long double ld = 0.1L;
    unsigned long big = 18446744073709551615ul;
    __int128 w = (__int128)1 << 100;
    int n;
    z = x / y;
    show("third", z);
    show("sum", z + z + z);
    show("neg", -z);
    show("muladd", mul_add(2, 3.5q, 0.25));
    show("many", many(0.5, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
    show("ld", ld);
    show("big", big);
    show("wide", w);
    show("table", table[0] + table[1] + table[2] + gq);
    show("strto", strtof128("3.14159265358979323846264338327950288", NULL));
    z = 10;
    z += 2.5;
    z *= 2;
    z -= 1;
    z /= 4;
    z++;
    ++z;
    z--;
    show("ops", z);
    n = (int)(z * 3);
    printf("%d %d %d %d %d %d\n", n, z > 6, z < 6, z == z, x != y, !x);
    printf("%g %g %Lg %ld %lu %d\n", (double)z, (float)(z / 3), (long double)(z / 3), (long)(-z), (unsigned long)(z * 1e15q), (int)(_Bool)z);
    if (z) printf("true\n");
    x = 0;
    printf("%s\n", x ? "nonzero" : "zero");
    return 0;
}
