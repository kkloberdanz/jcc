/*
 * Register allocation (Step 18): more live values than the machine has
 * registers, locals live across calls, nested loops left by goto,
 * setjmp/longjmp with a volatile counter, and the operand forms where
 * the destination register is also an operand. Compared against cc.
 */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static int calls = 0;

static int bump(int x) {
    calls++;
    return x + calls;
}

static double fbump(double x) {
    calls++;
    return x * 2.0 + calls;
}

/* thirty locals initialized before a call and read after it */
static long many_locals(int base) {
    int a0 = base + 0, a1 = base + 1, a2 = base + 2, a3 = base + 3, a4 = base + 4;
    int a5 = base + 5, a6 = base + 6, a7 = base + 7, a8 = base + 8, a9 = base + 9;
    int b0 = base * 2, b1 = base * 3, b2 = base * 4, b3 = base * 5, b4 = base * 6;
    int b5 = base * 7, b6 = base * 8, b7 = base * 9, b8 = base * 10, b9 = base * 11;
    int c0 = base - 1, c1 = base - 2, c2 = base - 3, c3 = base - 4, c4 = base - 5;
    int c5 = base - 6, c6 = base - 7, c7 = base - 8, c8 = base - 9, c9 = base - 10;
    long s;
    bump(a0);
    bump(b0);
    s = a0 + a1 * 2 + a2 * 3 + a3 * 4 + a4 * 5 + a5 * 6 + a6 * 7 + a7 * 8 + a8 * 9 + a9 * 10;
    s += bump(1);
    s = s * 31 + b0 - b1 + b2 - b3 + b4 - b5 + b6 - b7 + b8 - b9;
    s += bump(2);
    s = s * 17 + c0 * c1 + c2 * c3 + c4 * c5 + c6 * c7 + c8 * c9;
    return s;
}

/* a long expression with a call in the middle: every operand loaded before the call is live across it */
static long long_expression(const int *p) {
    return (p[0] * p[1] + p[2]) * (p[3] - p[4]) + (p[5] * p[6] - p[7]) * (p[8] + p[9])
        + (p[10] * p[11] + p[12]) * (p[13] - p[14]) + (p[15] * p[16] - p[17]) * (p[18] + p[19])
        + bump(p[20])
        + (p[21] * p[22] + p[23]) * (p[24] - p[25]) + (p[26] * p[27] - p[28]) * (p[29] + p[30])
        + (p[0] + p[31]) * (p[1] - p[32]) * (p[2] + p[33]) - (p[3] * p[34] - p[4] * p[35]);
}

/* floating point: no callee-saved vector registers on x86_64, eight on arm64 */
static double many_doubles(double x) {
    double d0 = x, d1 = x + 1, d2 = x + 2, d3 = x + 3, d4 = x + 4, d5 = x + 5;
    double d6 = x * 2, d7 = x * 3, d8 = x * 4, d9 = x * 5, d10 = x * 6, d11 = x * 7;
    double r;
    fbump(d0);
    r = d0 + d1 * d2 - d3 / d4 + d5 * d6 - d7 + d8 / d9 - d10 * d11;
    r = fbump(r) - d0 - d1 - d2 - d3 - d4 - d5 - d6 - d7 - d8 - d9 - d10 - d11;
    return r;
}

/* nested loops left by goto, with the loop variables read afterwards */
static int nested(int n) {
    int i, j, k;
    int sum = 0;
    for (i = 0; i < n; i++) {
        for (j = 0; j < n; j++) {
            for (k = 0; k < n; k++) {
                sum += i * j + k;
                if (sum > 1000) {
                    goto out;
                }
            }
        }
    }
out:
    return sum * 1000 + i * 100 + j * 10 + k;
}

static jmp_buf env;

static void jump_back(int v) {
    longjmp(env, v);
}

/* setjmp with a volatile counter; `kept` is not modified after setjmp, so it must survive the longjmp */
static int jumps(void) {
    volatile int count = 0;
    int kept = bump(100);
    int r;
    r = setjmp(env);
    count = count + 1;
    if (r < 3) {
        jump_back(r + 1);
    }
    return kept * 10 + count + r;
}

/* the destination is also an operand: x = y - x, y * x, y / x, shifts both ways */
static long hazards(long x, long y, unsigned u) {
    long t = 0;
    x = y - x;
    t += x;
    x = y * x;
    t += x;
    y = x - y;
    t += y;
    x = y - x;
    t += x;
    u = (unsigned)y << u;
    t += u;
    u = u >> (unsigned)(x & 3);
    t += u;
    y = y % (x | 1);
    t += y;
    x = (x & 0xff) | (y & 0xf00);
    t += x;
    x = y ^ x;
    return t + x;
}

static double fhazards(double a, double b) {
    double s = 0;
    a = b - a;
    s += a;
    a = b / a;
    s += a;
    b = a - b;
    s += b;
    b = b * a;
    s += b;
    a = b + a;
    return s + a;
}

/* a post-increment pointer walk and a swap through a temporary */
static int walk(const char *s) {
    int h = 0;
    int a = 1, b = 2, t;
    while (*s) {
        h = h * 31 + *s++;
        t = a;
        a = b;
        b = t + a;
    }
    return h + a * 3 + b;
}

/* narrow types promoted in registers */
static int narrow(signed char c, unsigned char uc, short sh, unsigned short us) {
    int r = 0;
    c = (signed char)(c + 100);
    uc = (unsigned char)(uc + 200);
    sh = (short)(sh * 1000);
    us = (unsigned short)(us * 1000);
    r += c;
    r += uc;
    r += sh;
    r += us;
    return r + (c < 0) + (sh < 0) + (us > 60000);
}

/* ten parameters, some on the stack on both targets, read after a call */
static long params(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j) {
    int k = bump(a);
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10 + k;
}

static int fib(int n) {
    int a, b;
    if (n < 2) {
        return n;
    }
    a = fib(n - 1);
    b = fib(n - 2);
    return a + b;
}

/* a struct whose address is taken lives in memory next to register locals */
struct point { int x, y; };

static void move_point(struct point *p, int dx) {
    p->x += dx;
    p->y -= dx;
}

static int points(int n) {
    struct point p;
    int i;
    int total = 0;
    p.x = n;
    p.y = -n;
    for (i = 0; i < n; i++) {
        move_point(&p, i);
        total += p.x * 2 + p.y;
    }
    return total;
}

/* more values live across a call than there are callee-saved registers: some must spill */
static long spills(int n) {
    int v[24];
    int i;
    long s = 0;
    for (i = 0; i < 24; i++) {
        v[i] = n * (i + 1);
    }
    {
        int a = v[0], b = v[1], c = v[2], d = v[3], e = v[4], f = v[5], g = v[6], h = v[7];
        int i2 = v[8], j = v[9], k = v[10], l = v[11], m = v[12], o = v[13], p = v[14], q = v[15];
        int r = v[16], t = v[17], u = v[18], w = v[19], x = v[20], y = v[21], z = v[22], aa = v[23];
        s = bump(a);
        s = s * 3 + a - b + c - d + e - f + g - h + i2 - j + k - l + m - o + p - q + r - t + u - w + x - y + z - aa;
        s = s * 3 + bump(b) + a * b + c * d + e * f + g * h + i2 * j + k * l + m * o + p * q + r * t + u * w + x * y + z * aa;
    }
    return s;
}

/* unsigned and signed division and remainder by registers, and comparisons materialized as values */
static int divisions(int a, int b, unsigned c, unsigned d) {
    int q = a / b;
    int r = a % b;
    unsigned uq = c / d;
    unsigned ur = c % d;
    int lt = a < b;
    int ge = c >= d;
    return q * 1000 + r * 100 + (int)uq * 10 + (int)ur + lt * 7 + ge * 3;
}

int main(void) {
    int p[36];
    int i;
    for (i = 0; i < 36; i++) {
        p[i] = (i * 7) % 11 - 5;
    }
    printf("many_locals %ld\n", many_locals(3));
    printf("long_expression %ld\n", long_expression(p));
    printf("many_doubles %.3f\n", many_doubles(1.5));
    printf("nested %d\n", nested(12));
    printf("jumps %d\n", jumps());
    printf("hazards %ld\n", hazards(7, 19, 3));
    printf("fhazards %.4f\n", fhazards(3.0, 10.0));
    printf("walk %d\n", walk("register allocation"));
    printf("narrow %d\n", narrow(100, 100, 40, 70));
    printf("params %ld\n", params(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
    printf("fib %d\n", fib(20));
    printf("points %d\n", points(10));
    printf("spills %ld\n", spills(3));
    printf("divisions %d\n", divisions(-47, 5, 47u, 5u));
    printf("calls %d\n", calls);
    return calls & 0x7f;
}
