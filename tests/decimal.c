/* _Decimal32, _Decimal64 and _Decimal128 (x86_64, BID as GCC encodes them): literals (df, dd, dl),
   static data, arithmetic, comparisons, conversions, and arguments and results of each */
#include <stdio.h>
#include <string.h>

static _Decimal32 g32 = 1.25DF;
static _Decimal64 g64[3] = { 0.1DD, -7, 2.5 };
static _Decimal128 g128 = 123456789012345678901234567890.123DL;

static void bytes(const char *what, const void *p, size_t n) {
    const unsigned char *b = p;
    printf("%s", what);
    while (n--) printf(" %02x", b[n]);
    printf("\n");
}

static _Decimal64 mul_add(_Decimal64 a, _Decimal64 b, _Decimal64 c) { return a * b + c; }
static _Decimal32 half32(_Decimal32 x) { return x / 2; }
static _Decimal128 sum128(_Decimal128 a, int n) { _Decimal128 s = 0; int i; for (i = 0; i < n; i++) s += a; return s; }

int main(void) {
    _Decimal64 a = 0.1DD, b = 0.2DD, c;
    _Decimal32 h = 1.1DF;
    _Decimal128 w = 1.000000000000000000000000000000001DL;
    long long n;
    bytes("g32", &g32, 4);
    bytes("g64", g64, 8);
    bytes("g64b", g64 + 1, 8);
    bytes("g128", &g128, 16);
    bytes("w", &w, 16);
    c = a + b;
    bytes("sum", &c, 8);
    printf("%d %d %d\n", c == 0.3DD, a < b, -a > b);
    c = mul_add(c, 10, -1);
    bytes("muladd", &c, 8);
    n = (long long)(c * 1000);
    printf("%lld %g %g\n", n, (double)c, (double)half32(h));
    printf("%g %d\n", (double)sum128(w, 3), (int)(g64[1] + g64[2]));
    c = 7;
    c /= 3;
    c++;
    printf("%.17g %d\n", (double)c, !c);
    h = (_Decimal32)c;
    bytes("narrow", &h, 4);
    w = c;
    bytes("wide", &w, 16);
    c = 1.5;
    bytes("fromdouble", &c, 8);
    return 0;
}
