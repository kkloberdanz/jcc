/* GNU vectors (__attribute__((vector_size(n)))): arithmetic element by element, with a scalar for
   every element, comparisons, casts of the bits, subscripts, initializers, __builtin_shuffle, and
   vectors of 4, 8, 16 and 32 bytes passed to and returned from functions */
#include <stdio.h>

typedef int v4si __attribute__((vector_size(16)));
typedef unsigned char v4qi __attribute__((vector_size(4)));
typedef short v4hi __attribute__((vector_size(8)));
typedef float v2sf __attribute__((vector_size(2 * sizeof(float))));
typedef double v4df __attribute__((vector_size(32)));
typedef long v2di __attribute__((vector_size(16)));
typedef int __attribute__((mode(SI))) si_t;

static v4si g = { 1, 2, 3, 4 };
static v4si garr[2] = { { 5, 6, 7, 8 }, { 9 } };

static v4si add(v4si a, v4si b) { return a + b; }
static v4qi inc(v4qi a) { return a + 1; }
static v4hi mul(v4hi a, short k) { return a * k; }
static v2sf half(v2sf a) { return a / 2; }
static v4df scale(v4df a, double k) { return a * k; }

static void show(const char *what, v4si v) {
    printf("%s %d %d %d %d\n", what, v[0], v[1], v[2], v[3]);
}

int main(void) {
    v4si a = { 10, -20, 30, -40 }, b = g, c;
    v4qi q = { 250, 1, 2, 255 };
    v4hi h = { 1, -2, 300, 4 };
    v2sf f = { 3.0f, 5.0f };
    v4df d = { 1, 2, 3, 4 };
    v2di l = (v2di)a;
    v4si mask = { 3, 2, 1, 0 };
    si_t s = 7;
    show("add", add(a, b));
    show("sub", a - b);
    show("mul", a * 3);
    show("div", a / b);
    show("mod", a % 7);
    show("and", a & 0xf0);
    show("or", a | b);
    show("xor", a ^ b);
    show("shl", b << 2);
    show("shr", a >> b);
    show("neg", -a);
    show("not", ~b);
    show("cmp", a > b);
    show("eq", a == (v4si){ 10, 0, 30, 0 });
    show("shuffle", __builtin_shuffle(a, mask));
    show("shuffle2", __builtin_shuffle(a, b, (v4si){ 0, 4, 1, 5 }));
    show("garr", garr[0] + garr[1]);
    c = a;
    c += b;
    c *= 2;
    c[1] = 99;
    show("compound", c);
    q = inc(q);
    printf("%d %d %d %d\n", q[0], q[1], q[2], q[3]);
    h = mul(h, 3);
    printf("%d %d %d %d\n", h[0], h[1], h[2], h[3]);
    f = half(f);
    printf("%g %g\n", f[0], f[1]);
    d = scale(d, 0.5);
    printf("%g %g %g %g\n", d[0], d[1], d[2], d[3]);
    printf("%lx %lx %d %d %d\n", (unsigned long)l[0], (unsigned long)l[1], (int)sizeof(v4df), (int)_Alignof(v4si), s);
    printf("%d\n", (int)(long long)(v2sf){ 0.0f, 0.0f });
    return 0;
}
