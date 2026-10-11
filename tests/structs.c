/*
 * Step 5 test: structs and unions passed and returned by value, in every
 * size class the ABIs distinguish: one eightbyte, two, and larger ones
 * that go through memory. Built with cc and jcc by tests/run.sh.
 */
#include <stdio.h>
#include <string.h>

struct S1 { char c; };
struct S3 { char a, b, c; };
struct S8 { int x, y; };
struct S12 { int x, y, z; };
struct S16 { long a, b; };
struct S24 { long a, b, c; };
struct S40 { char name[33]; int n; };
union U { int i; char c[3]; };

static int s1(struct S1 s) { return s.c; }
static int s3(struct S3 s) { return s.a * 100 + s.b * 10 + s.c; }
static int s8(struct S8 s) { return s.x * 10 + s.y; }
static int s12(struct S12 s) { return s.x * 100 + s.y * 10 + s.z; }
static long s16(struct S16 s) { return s.a * 10 + s.b; }
static long s24(struct S24 s) { return s.a * 100 + s.b * 10 + s.c; }
static int s40(struct S40 s) { return (int)strlen(s.name) + s.n; }
static int u(union U x) { return x.c[0] + x.c[2]; }

/* the callee modifies its copy; the caller's object must not change */
static int modify(struct S24 s, struct S8 t) {
    s.a = 99;
    t.x = 99;
    return (int)(s.a + t.x);
}

/* register exhaustion: a two eightbyte struct after seven scalars, then more */
static long spill(int a, int b, int c, int d, int e, int f, int g, struct S16 s, int h, struct S12 t, struct S24 big, int i) {
    return a + b + c + d + e + f + g + h + i + s.a * 1000 + s.b * 100 + t.x + t.y + t.z + big.c;
}

static struct S1 mk1(int c) { struct S1 s; s.c = (char)c; return s; }
static struct S3 mk3(int a, int b, int c) { struct S3 s; s.a = (char)a; s.b = (char)b; s.c = (char)c; return s; }
static struct S8 mk8(int x, int y) { struct S8 s; s.x = x; s.y = y; return s; }
static struct S12 mk12(int x, int y, int z) { struct S12 s; s.x = x; s.y = y; s.z = z; return s; }
static struct S16 mk16(long a, long b) { struct S16 s; s.a = a; s.b = b; return s; }
static struct S24 mk24(long a, long b, long c) { struct S24 s; s.a = a; s.b = b; s.c = c; return s; }
static struct S40 mk40(const char *name, int n) {
    struct S40 s;
    strcpy(s.name, name);
    s.n = n;
    return s;
}
static union U mku(int i) { union U x; x.i = i; return x; }

/* returning a parameter, and a struct from a pointer */
static struct S24 pass24(struct S24 s) { return s; }
static struct S8 deref8(struct S8 *p) { return *p; }

/* struct results fed straight into other calls */
static int chain(void) {
    return s8(mk8(1, 2)) + s24(mk24(1, 2, 3)) + s12(mk12(4, 5, 6)) + s3(mk3(7, 8, 9));
}

/* recursion with struct arguments and results */
static struct S16 fib(int n) {
    struct S16 r;
    if (n < 2) {
        r.a = n;
        r.b = 1;
        return r;
    }
    r = fib(n - 1);
    r.a += fib(n - 2).a;
    r.b++;
    return r;
}

typedef struct S12 (*Maker)(int, int, int);

struct Holder { struct S8 inner; struct S24 big; };

static struct Holder hold(struct S8 a, struct S24 b) {
    struct Holder h;
    h.inner = a;
    h.big = b;
    return h;
}

int main(void) {
    struct S1 a;
    struct S3 b;
    struct S8 c;
    struct S12 d;
    struct S16 e;
    struct S24 f;
    struct S40 g;
    union U v;
    struct S8 arr[3];
    Maker mk = mk12;
    struct Holder h;
    int i;

    a.c = 7;
    b.a = 1; b.b = 2; b.c = 3;
    c.x = 4; c.y = 5;
    d.x = 6; d.y = 7; d.z = 8;
    e.a = 9; e.b = 10;
    f.a = 11; f.b = 12; f.c = 13;
    strcpy(g.name, "forty");
    g.n = 40;
    v.i = 0x01020304;

    printf("%d %d %d %d %ld %ld %d %d\n", s1(a), s3(b), s8(c), s12(d), s16(e), s24(f), s40(g), u(v));
    printf("%d %ld %d\n", modify(f, c), f.a, c.x);
    printf("%ld\n", spill(1, 2, 3, 4, 5, 6, 7, e, 8, d, f, 9));

    a = mk1(42);
    b = mk3(3, 2, 1);
    c = mk8(8, 9);
    d = mk12(1, 2, 3);
    e = mk16(20, 21);
    f = mk24(30, 31, 32);
    g = mk40("longer name", 2);
    v = mku(0x0a0b0c0d);
    printf("%d %d %d %d %ld %ld %d %d\n", s1(a), s3(b), s8(c), s12(d), s16(e), s24(f), s40(g), u(v));
    printf("%d %ld %d %d\n", mk8(5, 6).y, mk24(7, 8, 9).c, mk12(1, 2, 3).z, mk40("abc", 1).n);
    printf("%ld %d\n", pass24(f).b, deref8(&c).x);
    printf("%d\n", chain());
    printf("%ld %ld\n", fib(10).a, fib(10).b);
    printf("%d\n", mk(9, 8, 7).x + mk(9, 8, 7).z);

    for (i = 0; i < 3; i++) {
        arr[i] = mk8(i, i * i);
    }
    printf("%d %d %d\n", s8(arr[0]), s8(arr[1]), s8(arr[2]));
    h = hold(c, f);
    printf("%d %ld %ld\n", h.inner.y, h.big.a, hold(mk8(1, 1), mk24(2, 2, 2)).big.c);
    return s3(b) + (int)s16(e);
}
