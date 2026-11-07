/* temporaries and parameters aligned past the 16 bytes a frame keeps: compound literals,
   parameters passed in registers, and a compound literal inside an over-aligned local's
   initializer (C89 cannot take the address of a call's record result) */
#include <stdio.h>

typedef unsigned long ul;
typedef int I32 __attribute__((aligned(32)));
typedef double D64 __attribute__((aligned(64)));
struct A { int a[8]; } __attribute__((aligned(32)));
struct P { int *p; int n; };

static int bad = 0;

static void check(const void *p, ul align, const char *what) {
    if ((ul)p % align) {
        printf("%s is misaligned\n", what);
        bad++;
    }
}

static struct A make(int x) {
    struct A r;
    int i;
    for (i = 0; i < 8; i++) {
        r.a[i] = x + i;
    }
    return r;
}

static int take_i32(char pad, I32 x) {
    check(&x, 32, "an I32 parameter");
    return pad + x;
}

static double take_d64(D64 d, char pad) {
    check(&d, 64, "a D64 parameter");
    return d + pad;
}

static int depth(int n, char *pad);

/* another 16 bytes of frame, so that every other level of depth lands 16 bytes over */
static int shifted(int n, char *pad) {
    char buf[16];
    int i, r;
    for (i = 0; i < 16; i++) {
        buf[i] = (char)i;
    }
    r = depth(n, pad);
    return r + buf[n & 15] - (n & 15);
}

static int depth(int n, char *pad) {
    struct A __attribute__((aligned(64))) local = { { n } };
    struct P p = { (struct A){ { n, 1, 2 } }.a, 3 };
    struct { struct P p; } __attribute__((aligned(64))) outer = { { (struct A){ { n, 5 } }.a, n } };
    int total;
    pad[0] = (char)n;
    check(&local, 64, "an over-aligned local");
    check(p.p, 32, "a compound literal");
    check(outer.p.p, 32, "a compound literal in an over-aligned local's initializer");
    check(&outer, 64, "an over-aligned local with a compound literal in it");
    total = make(n).a[0] + p.p[1] + outer.p.p[1] + outer.p.n + take_i32(pad[0], n) + (int)take_d64(n, 1);
    if (n > 0) {
        char more[3];
        total += n & 1 ? shifted(n - 1, more) : depth(n - 1, more);
    }
    return total;
}

int main(void) {
    char pad[5];
    int total = depth(6, pad);
    printf("%d %d\n", total, bad);
    return bad;
}
