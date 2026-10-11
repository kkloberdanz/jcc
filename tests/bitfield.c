/*
 * Step 5 test: bit-fields. Loads, stores, compound assignment, increment,
 * static and local initializers, signed and unsigned fields, fields in
 * 64 bit units and in unions.
 */
#include <stdio.h>

struct Flags {
    unsigned a : 3;
    unsigned b : 5;
    int c : 4;
    unsigned : 0;           /* pad to the next unit */
    unsigned d : 12;
    signed char e : 3;
    unsigned char f : 2;
    unsigned long g : 40;
    unsigned long h : 24;
    int i;
};

union U {
    unsigned x : 7;
    unsigned char raw;
};

static struct Flags gf = { 5, 17, -3, 4000, -2, 3, 0x1234567890ul, 0xabcdef, 99 };
static struct Flags gz;
static union U gu = { 100 };

static void show(const char *tag, struct Flags *f) {
    printf("%s: %u %u %d %u %d %u %lx %lx %d\n", tag, f->a, f->b, f->c, f->d, f->e, f->f, f->g, (unsigned long)f->h, f->i);
}

int main(void) {
    struct Flags l = { 1, 2, 3, 4, 1, 1, 1, 1, 1 };
    struct Flags *p = &l;
    union U u;
    unsigned long sz = sizeof(struct Flags);
    int v;

    show("gf", &gf);
    show("gz", &gz);
    show("l", &l);
    printf("%lu %u %u\n", sz, gu.x, gu.raw);

    /* plain stores, with truncation and sign */
    l.a = 9;                /* 9 & 7 = 1 */
    l.b = 255;
    l.c = 7;
    p->d = 70000;
    p->e = 5;               /* wraps to -3 */
    l.f = 7;
    l.g = 0xffffffffffful;
    l.h = 1 << 23;
    show("stores", &l);

    /* the value of an assignment is the stored value */
    v = (l.c = 9);
    printf("%d %d\n", v, l.c);
    v = (l.a = 7) + (l.b = 1);
    printf("%d\n", v);

    /* compound assignment and increments */
    l.a += 1;
    l.b -= 3;
    l.c *= -1;
    l.d |= 0xf00;
    l.e <<= 1;
    l.g >>= 4;
    l.h &= 0xff;
    show("compound", &l);
    l.a++;
    ++l.b;
    l.c--;
    --p->d;
    v = l.a++;
    printf("%d %u\n", v, l.a);
    v = p->b--;
    printf("%d %u\n", v, l.b);
    v = ++l.c;
    printf("%d %d\n", v, l.c);
    show("incdec", &l);

    /* wrap around on post increment: the old value is the one before wrapping */
    l.a = 7;
    v = l.a++;
    printf("%d %u\n", v, l.a);
    l.c = -8;
    v = l.c--;
    printf("%d %d\n", v, l.c);

    /* fields in expressions */
    printf("%d %d %d\n", l.a + l.b * 2, l.c < 0, gf.c == -3);
    printf("%u\n", gf.d + gf.a);
    v = gf.c;
    printf("%d %ld\n", v, (long)gf.g);

    u.raw = 0xff;
    printf("%u\n", u.x);
    u.x = 0;
    printf("%u\n", u.raw);
    return l.a + gf.a;
}
