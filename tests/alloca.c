/* alloca: the block lives until the function returns, whatever the expression depth */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <alloca.h>

struct S { int a; long b; char c[20]; };
struct Big { long v[8]; };
struct Small { int a; int b; };

static int sum(const char *p, int n) {
    int s = 0;
    int i;
    for (i = 0; i < n; i++) {
        s += p[i];
    }
    return s;
}

static long bigsum(const char *p, struct Big b) {
    int i;
    long s = 0;
    for (i = 0; i < 8; i++) {
        s += b.v[i];
    }
    return s + p[0];
}

static long smallsum(struct Small t, const char *p) {
    return t.a + t.b + p[0];
}

/* seven arguments pushed per level: three levels deep the alloca sees over 256 bytes of pushes */
static char *many(long a, long b, long c, long d, long e, long f, long g, char *p) {
    p[0] = (char)(p[0] + a + b + c + d + e + f + g);
    return p;
}

static int depth_sum(int n) {
    char *p = alloca(n + 1);
    memset(p, 'a', n);
    p[n] = 0;
    if (n == 0) {
        return 0;
    }
    return (int)strlen(p) + depth_sum(n - 1);
}

static int sizes(void) {
    char *keep[20];
    int i;
    int bad = 0;
    for (i = 0; i < 20; i++) {
        size_t n = (size_t)1 << (i % 12);
        keep[i] = alloca(n);
        memset(keep[i], i + 1, n);
    }
    for (i = 0; i < 20; i++) {
        size_t n = (size_t)1 << (i % 12);
        size_t k;
        for (k = 0; k < n; k++) {
            if (keep[i][k] != i + 1) {
                bad++;
            }
        }
        if (((unsigned long)keep[i] & 15) != 0) {
            printf("block %d is misaligned\n", i);
            bad++;
        }
    }
    return bad;
}

static int in_expressions(void) {
    int x = 5;
    int *p;
    struct S *s;
    char buf[8];
    struct Big b;
    struct Small t;
    int i;
    long r;
    for (i = 0; i < 8; i++) {
        b.v[i] = i * 10;
    }
    /* a pushed address outstanding during the alloca */
    p = (int *)alloca(sizeof *p);
    *p = 7;
    s = (struct S *)__builtin_alloca(sizeof *s);
    s->a = 1; s->b = 2; strcpy(s->c, "twenty");
    /* a pushed value outstanding: x + ... */
    r = x + (long)(((char *)alloca(32)) != 0);
    /* inside the arguments of calls, nested */
    r += sum(strcpy(alloca(100), "abc"), 3);
    r += sum(memset(alloca(16), 'z', 16), 16) + sum(strcpy(alloca(4), "q"), 1);
    /* records by value and an alloca block in the same call (a large record is passed
       by reference on arm64, so it must come after the alloca; a small one goes in registers) */
    r += bigsum(strcpy(alloca(8), "A"), b);
    t.a = 3; t.b = 4;
    r += smallsum(t, strcpy(alloca(8), "B"));
    r += many(1, 2, 3, 4, 5, 6, 7, many(1, 2, 3, 4, 5, 6, 7, many(1, 2, 3, 4, 5, 6, 7, strcpy(alloca(8), "C"))))[0];
    r += snprintf(buf, sizeof buf, "%d", *((int *)memset(alloca(4), 0, 4)) + 1);
    return (int)r + *p + s->a + (int)s->b + (int)strlen(s->c);
}

int main(void) {
    char *z = alloca(0);
    printf("depth_sum %d\n", depth_sum(40));
    printf("sizes %d\n", sizes());
    printf("in_expressions %d\n", in_expressions());
    printf("zero %d\n", z != NULL);
    return 0;
}
