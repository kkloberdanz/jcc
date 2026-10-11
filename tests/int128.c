/* __int128 and unsigned __int128: arithmetic, shifts, comparisons, conversions, calls, varargs, static data */
#include <stdarg.h>
#include <stdio.h>

typedef __int128 i128;
typedef unsigned __int128 u128;

static i128 g = -5;
static u128 gu = 12345678901234567890ull;
static i128 garr[3] = { 1, -2, 3 };

static void print128(const char *tag, u128 v) {
    unsigned long hi = (unsigned long)(v >> 64);
    unsigned long lo = (unsigned long)v;
    printf("%s: %lx %lx\n", tag, hi, lo);
}

static i128 add3(i128 a, i128 b, i128 c) { return a + b + c; }
static u128 mulu(u128 a, u128 b) { return a * b; }
static i128 divs(i128 a, i128 b) { return a / b; }
static i128 mods(i128 a, i128 b) { return a % b; }
static u128 divu(u128 a, u128 b) { return a / b; }
static u128 modu(u128 a, u128 b) { return a % b; }
static i128 shl(i128 a, int n) { return a << n; }
static i128 shr(i128 a, int n) { return a >> n; }
static u128 shru(u128 a, int n) { return a >> n; }
/* many parameters: the pair must land in an even register pair and then on the stack */
static i128 many(int a, i128 b, int c, i128 d, int e, i128 f, i128 h, int i, i128 j) {
    return a + b + c + d + e + f + h + i + j;
}
static double tod(i128 a) { return (double)a; }
static float tof(u128 a) { return (float)a; }
static i128 fromd(double d) { return (i128)d; }
static u128 fromf(float f) { return (u128)f; }
static i128 sumv(int n, ...) {
    va_list ap;
    i128 s = 0;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) {
        s += va_arg(ap, i128);
    }
    va_end(ap);
    return s;
}

int main(void) {
    i128 a = 1;
    u128 u = 1;
    i128 b;
    int i;
    i128 neg = -1;
    u128 big = ~(u128)0;
    struct { char c; i128 x; } s;

    a <<= 100;
    print128("1<<100", (u128)a);
    print128("g", (u128)g);
    print128("gu", gu);
    print128("garr", (u128)(garr[0] + garr[1] * garr[2]));
    b = a + 7;
    print128("a+7", (u128)b);
    print128("b-a", (u128)(b - a));
    print128("neg", (u128)neg);
    print128("big", big);
    print128("big>>1", big >> 1);
    print128("big>>64", big >> 64);
    print128("big>>127", big >> 127);
    print128("neg>>1", (u128)(neg >> 1));
    print128("neg>>100", (u128)(neg >> 100));
    print128("shl 63", (u128)shl(1, 63));
    print128("shl 64", (u128)shl(1, 64));
    print128("shl 0", (u128)shl(3, 0));
    print128("shr 0", (u128)shr(a, 0));
    print128("shr 37", (u128)shr(a, 37));
    print128("shr 99", (u128)shr(-a, 99));
    print128("shru 64", shru(big, 64));
    print128("mul", mulu(0xffffffffffffffffull, 0xffffffffffffffffull));
    print128("mul2", (u128)(a * 3 + (i128)-2 * a));
    print128("mulneg", (u128)((i128)-7 * (i128)1000000000000000000ll * 1000));
    print128("div", (u128)divs(a, 3));
    print128("divneg", (u128)divs(-a, 7));
    print128("mod", (u128)mods(a, 1000000007));
    print128("modneg", (u128)mods(-a, 1000000007));
    print128("divu", divu(big, 10));
    print128("modu", modu(big, 1000000000000ull));
    print128("add3", (u128)add3(a, -a, 42));
    print128("many", (u128)many(1, 2, 3, 4, 5, 6, 7, 8, 9));
    print128("many2", (u128)many(1, a, 3, -a, 5, g, 7, 8, a));
    print128("and", (u128)(b & 0xff));
    print128("or", (u128)(a | 1));
    print128("xor", (u128)(b ^ a));
    print128("not", (u128)~a);
    print128("negate", (u128)-a);
    printf("cmp %d %d %d %d %d %d\n", a < b, a > b, a <= a, a >= b, a == a, a != b);
    printf("cmpneg %d %d %d %d\n", neg < a, neg < 0, (u128)neg < (u128)a, (u128)neg > (u128)a);
    printf("cmpu %d %d %d\n", big > u, big == ~u + u, u <= big);
    printf("bool %d %d %d %d\n", !a, !!a, !(a - a), (a << 20) ? 1 : 0);
    if (a) printf("truthy\n");
    if (a - a) printf("wrong\n"); else printf("falsy\n");
    printf("conv %.0f %.0f %.0f\n", tod(a), tod(neg), (double)big);
    printf("convf %g %g\n", (double)tof(gu), (double)tof(big));
    print128("fromd", (u128)fromd(1e30));
    print128("fromdneg", (u128)fromd(-1e30));
    print128("fromf", fromf(3e20f));
    print128("sumv", (u128)sumv(4, a, (i128)1, (i128)-3, g));
    for (i = 0; i < 3; i++) {
        a++;
        u += 2;
    }
    print128("inc", (u128)a);
    print128("incu", u);
    b = a++;
    print128("post", (u128)b);
    b = --a;
    print128("pre", (u128)b);
    s.x = -12345;
    s.c = 'q';
    s.x *= 3;
    print128("member", (u128)s.x);
    printf("narrow %d %ld %u\n", (int)s.x, (long)(a >> 100), (unsigned)big);
    printf("size %d %d\n", (int)sizeof(i128), (int)sizeof(s));
    return (int)(a >> 100);
}
