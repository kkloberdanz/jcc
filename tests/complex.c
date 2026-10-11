/* _Complex float, double and long double: operators, conversions, calls, records, va_arg, static data */
#include <complex.h>
#include <stdarg.h>
#include <stdio.h>

typedef double _Complex cd;
typedef float _Complex cf;
typedef long double _Complex cl;

struct Rec { int tag; cf f; cd d; };

static cd gd = 1.5 + 2.5 * I;
static cf gf = 3.0f - 4.0if;
static cl gl = 2.0L + 1.0iL;
static cd garr[2] = { -1.0i, (2.0 + 3.0i) * (1.0 - 1.0i) };
static struct Rec grec = { 7, 1.0f + 1.0fi, 2.0 };
static cd gneg = -(1.0 + 2.0i);
static cd gfromreal = 5;

static void show(const char *what, cd z) {
    printf("%s %g %g\n", what, __real__ z, __imag__ z);
}

static void showf(const char *what, cf z) {
    printf("%s %g %g\n", what, (double)__real__ z, (double)__imag__ z);
}

static void showl(const char *what, cl z) {
    printf("%s %.6Lg %.6Lg\n", what, __real__ z, __imag__ z);
}

static cd add(cd a, cd b) { return a + b; }
static cf mulf(cf a, cf b) { return a * b; }
static cl divl(cl a, cl b) { return a / b; }
static cd scale(cd z, double k) { return z * k; }
static double re_of(cd z) { return __real__ z; }

static struct Rec make_rec(int tag, cf f, cd d) {
    struct Rec r;
    r.tag = tag;
    r.f = f;
    r.d = d;
    return r;
}

static cd vsum(int n, ...) {
    va_list ap;
    cd s = 0;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) {
        if (i % 3 == 0) {
            s += va_arg(ap, cd);
        } else if (i % 3 == 1) {
            s += va_arg(ap, cf);
        } else {
            s += va_arg(ap, cl);
        }
    }
    va_end(ap);
    return s;
}

/* enough arguments to spill complex values onto the stack */
static cd many(cd a, cd b, cd c, cd d, cd e, cf f, cf g, cf h, cf i, cf j, int k, cd l) {
    return a + b + c + d + e + f + g + h + i + j + k + l;
}

static int operators(void) {
    cd a = 1.0 + 2.0i;
    cd b = 3.0 - 1.0i;
    cd c;
    cf fa = 1.5f + 0.5if;
    cf fb = 2.0f;
    cl la = 1.0L + 1.0iL;
    int n = 0;
    show("add", a + b);
    show("sub", a - b);
    show("mul", a * b);
    show("div", a / b);
    show("neg", -a);
    show("pos", +b);
    show("real op complex", 2.0 * a);
    show("complex op int", a / 2);
    show("int op complex", 1 - a);
    showf("float mul", fa * fb);
    showf("float div", fa / fb);
    showl("long double mul", la * la);
    showl("long double div", la / (2.0L + 1.0iL));
    c = a;
    c += b;
    show("add assign", c);
    c -= 1.0i;
    show("sub assign", c);
    c *= 2;
    show("mul assign", c);
    c /= a;
    show("div assign", c);
    fa += a;    /* through the common type and back */
    showf("float add assign double", fa);
    printf("eq %d %d %d %d\n", a == b, a != b, a == 1.0 + 2.0i, a != a);
    printf("eq real %d %d %d\n", b == 3.0, (cd)3 == 3, a == 1.0);
    printf("truth %d %d %d %d\n", !a, !(cd)0, a && b, (cd)0 || 0.0i);
    if (a) {
        n++;
    }
    if (!(a - a)) {
        n++;
    }
    while (c) {
        c = 0;
        n++;
    }
    printf("cond %d\n", n);
    show("ternary", n ? a : b);
    show("comma", (n++, b));
    printf("sizeof %d %d %d\n", (int)sizeof(cf) == 2 * (int)sizeof(float), (int)sizeof(cd) == 2 * (int)sizeof(double),
           (int)sizeof(cl) == 2 * (int)sizeof(long double));
    return n;
}

static int conversions(void) {
    cd a = 2.5 + 7.0i;
    cf f = a;               /* narrowing, imaginary kept */
    cl l = a;
    double d = a;           /* the real part */
    int i = a;
    cd fromint = 3;
    cd fromfloat = 1.25f;
    cf fromlong = 2L;
    showf("to float", f);
    showl("to long double", l);
    printf("to real %g %d %d\n", d, i, (int)(cf)(-3.75 + 1.0i));
    show("from int", fromint);
    show("from float", fromfloat);
    showf("from long", fromlong);
    show("cast", (cd)(1.0f + 2.0fi) + (cd)1.0L);
    printf("bool %d %d %d\n", (int)(_Bool)a, (int)(_Bool)(cd)0, (int)(_Bool)(0.0 + 1e-300i));
    return 0;
}

static int components(void) {
    cd z = 0;
    cf w;
    cd arr[2];
    double r = 4.5;
    __real__ z = 1.25;
    __imag__ z = -2.5;
    __real__ w = 1.0f;
    __imag__ w = 2.0f;
    __real__ arr[1] = 3;
    __imag__ arr[1] = 4;
    __imag__ z *= 2;
    __real__ z += __imag__ w;
    show("set", z);
    showf("setf", w);
    show("array", arr[1]);
    printf("rvalue parts %g %g %g %g\n", __real__ (z * 2), __imag__ (z + w), __real__ r, __imag__ r);
    printf("builtins %g %g %g\n", __builtin_creal(z), __builtin_cimag(z), (double)__builtin_cimagf(w));
    show("conj", __builtin_conj(z));
    showf("conjf", __builtin_conjf(w));
    show("creal", creal(z) + 0.0i);
    show("conj lib", conj(z));
    return 0;
}

static int calls(void) {
    cd a = 1.0 + 1.0i;
    cf f = 2.0f + 3.0if;
    cl l = 4.0L + 5.0iL;
    struct Rec r = make_rec(9, f, a);
    show("call", add(a, 2.0 - 0.5i));
    showf("callf", mulf(f, f));
    showl("calll", divl(l, 2.0L + 1.0iL));
    show("scale", scale(a, 3));
    printf("re_of %g\n", re_of(a * a));
    printf("rec %d\n", r.tag);
    showf("rec f", r.f);
    show("rec d", r.d);
    show("vsum", vsum(6, a, f, l, 1.0 + 0.0i, 0.5if, 0.25L + 0.0iL));
    show("many", many(a, a, a, a, a, f, f, f, f, f, 10, 1.0i));
    show("global", gd);
    showf("globalf", gf);
    showl("globall", gl);
    show("garr0", garr[0]);
    show("garr1", garr[1]);
    printf("grec %d\n", grec.tag);
    showf("grec f", grec.f);
    show("grec d", grec.d);
    show("gneg", gneg);
    show("gfromreal", gfromreal);
    gd = gd * gd;
    show("global assign", gd);
    return 0;
}

int main(void) {
    cd units[3];
    int i;
    operators();
    conversions();
    components();
    calls();
    /* a loop long enough to show any stack imbalance */
    units[0] = 1.0i;
    for (i = 1; i < 3; i++) {
        units[i] = units[i - 1] * 1.0i;
    }
    show("i squared", units[1]);
    show("i cubed", units[2]);
    return 0;
}
