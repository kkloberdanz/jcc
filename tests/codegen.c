/*
 * Step 4 test: integer code generation. Built with cc and jcc by
 * tests/run.sh; the exit status and output must agree.
 */
#include <stdio.h>
#include <string.h>

struct Point { int x, y; };
struct Line { struct Point a, b; char name[8]; };
union Bits { unsigned u; unsigned char c[4]; };
enum Color { RED, GREEN = 5, BLUE };

typedef int (*Op)(int, int);

static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
static int mul(int a, int b) { return a * b; }

int counter;                            /* tentative definition */
static int table[] = { 1, 2, 3, 4 };
static const char *names[] = { "zero", "one", "two" };
static struct Line line = { { 1, 2 }, { 3, 4 }, "diag" };
static struct Point pts[3] = { { 1, 2 }, 3, 4, { 5 } };
static int *ptab = table + 1;
static int *pelem = &table[2];
static char *pname = &line.name[1];
static Op ops[] = { add, sub, mul, 0 };
static char greeting[] = "hi";
static char fixed[4] = "abc";
static int grid[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };
static long big = 1234567890123L;
static unsigned char bytes[4] = { 255, 1, 2 };
static short shorts[] = { -1, 32767 };
static void *nothing = (void *)0;

int next_id(void) {
    static int id = 100;
    return id++;
}

int sum9(int a, int b, int c, int d, int e, int f, int g, int h, int i) {
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9;
}

long mixed(char c, short s, unsigned char uc, unsigned short us, long l) {
    return c + s + uc + us + l;
}

int fact(int n) {
    if (n <= 1) {
        return 1;
    }
    return n * fact(n - 1);
}

int kr(a, b)
    int a;
    char b;
{
    return a * 10 + b;
}

static int classify(int n) {
    int r = 0;
    switch (n) {
        case 0:
            r = 100;
            break;
        case 1:
        case 2:
            r = 200;
            /* fall through */
        case 3:
            r += 1;
            break;
        case -1:
            r = -100;
            break;
        default:
            r = 999;
    }
    return r;
}

static int apply(Op op, int a, int b) {
    return op(a, b);
}

static void fill(int *p, int n, int v) {
    int i;
    for (i = 0; i < n; i++) {
        p[i] = v + i;
    }
}

static int strlen_(const char *s) {
    const char *p = s;
    while (*p) {
        p++;
    }
    return p - s;
}

static unsigned hash(const char *s) {
    unsigned h = 5381;
    int c;
    while ((c = *s++) != 0) {
        h = h * 33 + (unsigned)c;
    }
    return h;
}

int main(void)
{
    int i = 7, j = -3;
    unsigned u = 7;
    long l = 1L << 40;
    unsigned long ul = (unsigned long)-1;
    char c = 'A';
    signed char sc = -5;
    unsigned char uc = 250;
    short sh = -300;
    unsigned short ush = 60000;
    int a[5];
    int *p;
    struct Point pt = { 10, 20 };
    struct Point pt2;
    struct Line ln;
    union Bits bits;
    char buf[16] = "init";
    int grid2[2][2] = { { 1, 2 }, { 3 } };
    enum Color color = GREEN;
    int n;

    /* arithmetic, signedness and width */
    printf("%d %d %d %d %d\n", i + j, i - j, i * j, i / j, i % j);
    printf("%d %d %d\n", -7 / 2, -7 % 2, 7 / -2);
    printf("%u %u %u\n", u / 2u, u * 3u, 0u - u);
    printf("%ld %lu %ld\n", l, ul, l >> 3);
    printf("%d %d %d %d\n", c, sc, uc, sh);
    printf("%d %u %d\n", ush, ush, (short)ush);
    printf("%d %d\n", (unsigned char)sc, (signed char)uc);
    printf("%d %d %d %d\n", i << 2, i >> 1, j >> 1, -1 >> 1);
    printf("%u %u\n", (unsigned)-1 >> 28, ~0u >> 1);
    printf("%d %d %d\n", i & 3, i | 8, i ^ 5);
    printf("%d %d\n", ~i, -i);
    printf("%d %d %d %d %d %d\n", i < j, i > j, i <= 7, i >= 8, i == 7, i != 7);
    printf("%d %d\n", u < (unsigned)j, (int)u < j);
    printf("%d %d %d %d\n", !i, !0, i && j, 0 || j);
    printf("%d\n", (i, j, 42));
    printf("%d %d\n", i > j ? i : j, i < j ? i : j);
    printf("%d %d\n", color, BLUE + RED);
    printf("%ld %ld\n", (long)sizeof(struct Line), (long)sizeof a);
    printf("%d\n", 2147483647 + 0);
    printf("%d\n", -2147483647 - 1);

    /* compound assignment and increment */
    i += 3; i -= 1; i *= 2; i /= 3; i %= 4;
    printf("%d\n", i);
    i <<= 3; i >>= 1; i &= 13; i |= 32; i ^= 1;
    printf("%d\n", i);
    i = -1;
    i /= 2u;            /* done in unsigned int, then back to int */
    printf("%d\n", i);
    uc = 250;
    uc += 10;           /* wraps to 4 */
    sh = 32767;
    sh++;               /* wraps to -32768 */
    printf("%d %d\n", uc, sh);
    i = 5;
    j = i++;
    printf("%d %d\n", j, i);
    j = ++i;
    printf("%d %d\n", j, i);
    j = i--;
    printf("%d %d\n", j, i);
    j = --i;
    printf("%d %d\n", j, i);

    /* pointers and arrays */
    fill(a, 5, 10);
    p = a;
    printf("%d %d %d\n", *p, p[2], *(p + 4));
    p += 2;
    printf("%d\n", *p);
    i = *--p;
    printf("%d %ld\n", i, (long)(p - a));
    *p++ = 77;
    printf("%d %d %d\n", a[1], *p, 3[a]);
    printf("%d %d\n", p > a, p == a + 2);
    printf("%d\n", strlen_("hello"));
    printf("%u\n", hash("hello world"));
    printf("%s %s %s\n", buf, greeting, fixed);
    strcpy(buf, "changed");
    printf("%s %d\n", buf, (int)strlen(buf));
    printf("%d %d %d %d\n", grid2[0][0], grid2[0][1], grid2[1][0], grid2[1][1]);
    printf("%d %d %d\n", grid[1][2], *ptab, *pelem);
    printf("%s %s %c\n", names[1], names[2], *pname);
    printf("%d %d %d\n", bytes[0], bytes[1], bytes[3]);
    printf("%d %d\n", shorts[0], shorts[1]);
    printf("%ld %d\n", big, nothing == 0);

    /* structs and unions */
    printf("%d %d\n", pt.x, pt.y);
    pt2 = pt;
    pt2.x += 5;
    printf("%d %d %d\n", pt2.x, pt2.y, pt.x);
    ln = line;
    ln.b.y = 99;
    printf("%d %d %d %d %s %s\n", ln.a.x, ln.a.y, ln.b.x, ln.b.y, ln.name, line.name);
    printf("%d %d %d %d %d %d\n", pts[0].x, pts[0].y, pts[1].x, pts[1].y, pts[2].x, pts[2].y);
    bits.u = 0x01020304;
    printf("%d\n", bits.c[0] + bits.c[1] * 2 + bits.c[2] * 3 + bits.c[3] * 4);
    {
        struct Point *pp = &pt;
        pp->x = 1;
        (*pp).y = 2;
        printf("%d %d\n", pt.x, pt.y);
    }

    /* globals and statics */
    counter = 5;
    counter++;
    i = next_id();
    j = next_id();
    printf("%d %d %d %d\n", counter, i, j, next_id());

    /* calls */
    printf("%d\n", sum9(1, 2, 3, 4, 5, 6, 7, 8, 9));
    printf("%ld\n", mixed(-1, -2, 255, 65535, 1000000000000L));
    printf("%d %d\n", fact(10), kr(4, 'x'));
    printf("%d %d %d\n", apply(ops[0], 6, 3), apply(ops[1], 6, 3), apply(ops[2], 6, 3));
    for (n = 0; ops[n]; n++) {
        printf("%d ", ops[n](n, 2));
    }
    printf("\n");
    printf("%d %d %d %d %d\n", classify(0), classify(1), classify(3), classify(-1), classify(42));

    /* control flow */
    n = 0;
    while (n < 100) {
        n += 7;
        if (n % 2) {
            continue;
        }
        if (n > 50) {
            break;
        }
    }
    printf("%d\n", n);
    n = 0;
    do {
        n++;
    } while (n < 3);
    printf("%d\n", n);
    for (i = 0, j = 10; i < j; i++, j--) {
        ;
    }
    printf("%d %d\n", i, j);
    n = 0;
    for (;;) {
        if (++n == 4) {
            goto done;
        }
    }
done:
    printf("%d\n", n);
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            if (j == 2) {
                break;
            }
            n += j;
        }
    }
    printf("%d\n", n);

    return counter + 3;
}
