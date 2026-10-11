/*
 * Step 3 test: types, conversions, initializers, constant expressions.
 * `jcc -fsyntax-only tests/sema.c` must be silent; built with cc it exits 0.
 * It uses floating point, which the code generator does not do yet.
 * jcc: syntax-only
 */

struct Point { int x, y; };
struct Line { struct Point a, b; char name[8]; };
union Num { int i; double d; char c[4]; };
enum Color { RED, GREEN = 5, BLUE };

typedef int (*Op)(int, int);
typedef struct Point Point;

static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }

/* static initializers: constants, address constants, brace elision, strings */
static int table[] = { 1, 2, 3, 4 };
static const char *names[] = { "zero", "one", "two" };
static struct Line line = { { 1, 2 }, { 3, 4 }, "diag" };
static struct Line line2 = { 1, 2, 3, 4, { 'x', 'y' } };
static struct Point pts[3] = { { 1, 2 }, 3, 4, { 5 } };
static int *ptab = table + 1;
static int *pelem = &table[2];
static char *pname = &line.name[1];
static Op ops[] = { add, sub, 0 };
static char greeting[] = "hi";
static char fixed[4] = "abc";
static int grid[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };
static int flat[2][3] = { 1, 2, 3, 4, 5, 6 };
static union Num num = { 7 };
static void *vp = (void *)0;
static long big = 4294967295U * 2UL;
static enum Color color = BLUE;
static unsigned char uc = 255;
static double dbl = 1 / 2.0;
static int neg = -(int)sizeof(struct Point);

/* array sizes and case labels from constant expressions */
#define N (sizeof(table) / sizeof(table[0]))
static char buf[N * 2];
static char bits[(1 << 3) + (7 & 3)];

int variadic(int n, ...);
int kr(a, b) char a; double b; { return a + (int)b; }

static int classify(int c)
{
    switch (c) {
        case 'a': case 'b': return 1;
        case RED + 1: return 2;
        case (int)sizeof(int) * 2: return 3;
        default: return 0;
    }
}

static Point *first(Point *p, int n)
{
    return n > 0 ? p : (Point *)0;
}

int main(void)
{
    int i = 0;
    unsigned u = 3;
    long l = 1;
    char c = 'c';
    short s = -2;
    float f = 2.5f;
    double d = 1.5;
    int *ip = &i;
    const int *cip = ip;
    void *vp2 = ip;
    char *cp = greeting;
    struct Line *lp = &line;
    Point p = line.a;
    Op op = add;
    int (*fp)(int, int) = &sub;
    int arr[3] = { 1, 2, 3 };
    int sum = 0;
    int j;
    int k = sizeof(k) + sizeof k;

    /* usual arithmetic conversions, promotions */
    if (sizeof(c + c) != sizeof(int)) return 1;
    if (sizeof(s * 2) != sizeof(int)) return 2;
    if (sizeof(i + l) != sizeof(long)) return 3;
    if (sizeof(u + l) != sizeof(long)) return 4;
    if (sizeof(f + i) != sizeof(float)) return 5;
    if (sizeof(f + d) != sizeof(double)) return 6;
    if (sizeof(c ? i : u) != sizeof(unsigned)) return 7;
    if (sizeof(ip - ip) != sizeof(long)) return 8;
    if (sizeof(&arr) != sizeof(int *)) return 9;
    if (sizeof arr != 12) return 10;
    if (sizeof(line.name) != 8) return 11;
    if (sizeof greeting != 3) return 12;
    if (sizeof(ops) / sizeof(ops[0]) != 3) return 13;
    if (sizeof(pts) != 3 * sizeof(Point)) return 14;
    if (N != 4 || sizeof(buf) != 8 || sizeof(bits) != 11) return 15;
    if (sizeof(grid) != sizeof(flat)) return 16;
    if (sizeof(num) != 8) return 17;

    /* pointer arithmetic and decay */
    sum = 0;
    for (j = 0; j < 3; j++) {
        sum += arr[j] + *(arr + j) + j[arr];
    }
    if (sum != 18) return 18;
    if (ptab - table != 1 || *pelem != 3 || *pname != 'i') return 19;
    if (lp->a.x != 1 || (*lp).b.y != 4 || lp->name[0] != 'd') return 20;
    if (line2.b.y != 4 || line2.name[1] != 'y') return 21;
    if (pts[1].x != 3 || pts[1].y != 4 || pts[2].x != 5 || pts[2].y != 0) return 22;
    if (flat[1][2] != 6 || grid[1][2] != 6) return 23;
    if (fixed[3] != 0 || names[2][1] != 'w') return 24;
    if (num.i != 7) return 25;

    /* calls through pointers, function designators, K&R */
    if (op(2, 3) != 5 || (*fp)(5, 3) != 2 || ops[1](9, 4) != 5 || (*ops[0])(1, 1) != 2) return 26;
    if (kr('a', 1.9) != 'a' + 1) return 27;
    if (classify('b') != 1 || classify(1) != 2 || classify(8) != 3 || classify(9) != 0) return 28;
    if (first(&p, 1) != &p || first(&p, 0) != 0) return 29;

    /* assignment conversions, compound assignment, increment */
    i = c;
    i += d;
    l = u;
    u = -1;
    c = 300;
    s = l;
    d = i;
    f = l;
    i = f;
    cip = &i;
    vp2 = cip == ip ? vp2 : 0;
    p = lp->a.x ? line.a : line.b;
    p.x++;
    ++p.y;
    cp += 1;
    cp = cp - 1;
    ip = (int *)vp2;
    if (i != 3 || u != 4294967295U || s != 3 || c != 44 || p.x != 2 || p.y != 3 || *cp != 'h') return 30;
    if (cip != &i || ip != &i) return 31;

    /* enums, conditional, comma, logical */
    color = GREEN;
    i = (color == GREEN) + (BLUE - RED) + (RED ? 1 : 2) + (j = 1, j + 1);
    if (i != 1 + 6 + 2 + 2) return 32;
    i = !ip + !!vp + (ip && cp) + (0 || 2.5);
    if (i != 0 + 0 + 1 + 1) return 33;

    /* unsigned and shifts */
    u = 1u << 31;
    l = (long)u << 1;
    if ((u >> 31) != 1 || l != 4294967296L || (-8 >> 1) != -4 || big != 8589934590L) return 34;
    if (uc != 255 || dbl != 0.5 || neg != -8 || neg + (int)sizeof(Point) != 0) return 35;

    /* string literals are arrays, char constants are ints */
    if ("abc"[1] != 'b' || sizeof("abc") != 4 || sizeof('a') != sizeof(int)) return 36;
    if (variadic(3, 1, 2L, 3.0, "x") != 3) return 37;
    if (sum < 0) goto fail;
    return 0;
fail:
    return 99;
}

int variadic(int n, ...) { return n; }
