/* Promotions and constant evaluation the GCC torture tests caught: bit-fields
   promote by width (3.2.1.1), floating constant expressions are evaluated in
   their own type, a K&R float parameter arrives as a double, and hexadecimal
   floating constants. */
#include <stdio.h>

struct X {
    unsigned int u3 : 3;
    signed long s31 : 31;
    signed long s32 : 32;
    unsigned long u31 : 31;
    unsigned long u32 : 32;
    unsigned long long ull3 : 3;
    unsigned long long ull35 : 35;
    unsigned u15 : 15;
    long i24 : 24;
};

static float fx(x)
    float x;
{
    return 1.0 + 3.0 / (2.302585093 * x);
}

static int sum3(a, b, c)
    char a;
    short b;
    float c;
{
    return a + b + (int)c;
}

static double d = 1.17549435082228750797e-38F / 2.0;
static float f = 1.0F / 3.0F;

int main(void)
{
    struct X x;
    struct { signed i : 7; unsigned u : 7; } bit;
    int i = -13;
    unsigned u = 61;
    double dd = 1.17549435082228750797e-38F / 2.0;
    float ff = 1.0F / 3.0F;
    float fa = 3.0;
    double h = 0x1.8p3, hh = 0xA.8P-1;
    float hf = 0x1p-100f * 0x1p100f;

    x.u3 = 0; x.s31 = 0; x.s32 = 0; x.u31 = 0; x.u32 = 0; x.ull3 = 0; x.ull35 = 0; x.u15 = 0;
    printf("%d %d %d %d %d %d %d %d\n",
        (x.u3 - 2) < 0, (x.s31 - 2) < 0, (x.s32 - 2) < 0, (x.u15 - 2) < 0,
        (x.u31 - 2) < 0, (x.u32 - 2) < 0, (x.ull3 - 2) < 0, (x.ull35 - 2) < 0);
    x.i24 = 0xFEFEFEFE;
    printf("%d %lx\n", x.i24 == 0xFFFEFEFE, (unsigned long)x.i24);
    bit.u = 61; bit.i = -13;
    printf("%d %d %d %d %d %d\n", i % u == -13U % 61, i % bit.u == -13 % 61, bit.i % bit.u == -13 % 61,
        i % (unsigned)bit.u == -13U % 61, (int)sizeof(bit.u + 0), (int)sizeof(x.ull35 + 0));
    printf("%d %d %d %g\n", d == dd, f == ff, (float)(1.0 / 3.0) == 1.0F / 3.0F, d * 1e38);
    printf("%.4f %d\n", fx(4.0) + fa, sum3(1, 2, 3.5));
    printf("%g %g %g\n", h, hh, hf);
    return 0;
}
