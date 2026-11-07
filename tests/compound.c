/* C99 compound literals and empty initializers: automatic and static, arrays, records, scalars, as lvalues */
#include <stdio.h>
#include <string.h>

struct P { int x, y; };
struct S { int a; struct P p; double d; const char *name; };
struct E { };    /* GNU: no members, size 0 */

static int *gp = (int[]){ 10, 20, 30 };
static struct P *gpp = &(struct P){ 4, 5 };
static const char *const *names = (const char *const[]){ "zero", "one", "two" };
static struct P gcopy = (struct P){ .y = 9 };   /* GNU: a static record from a literal */
static struct { int n; int *v; } gtab[] = { { 2, (int[]){ 1, 2 } }, { 3, (int[]){ 7, 8, 9 } } };
static int gzero = {};
static struct S gs = {};
static int garr[4] = {};

static int sum(const int *v, int n) {
    int s = 0;
    while (n-- > 0) {
        s += *v++;
    }
    return s;
}

static struct P mid(struct P a, struct P b) {
    return (struct P){ (a.x + b.x) / 2, (a.y + b.y) / 2 };
}

static int area(const struct P *p) { return p->x * p->y; }

static double dot(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1]; }

int main(void) {
    int i;
    int *p;
    int *ptrs[3];
    struct P q;
    struct S s = { 1, { 2, 3 }, 4.5, "s" };
    struct E e = {};
    int z = {};
    double dz = {};
    char buf[8] = {};
    int counter;

    printf("%d %d %d\n", gp[0], gp[1], gp[2]);
    printf("%d %d %s %s\n", gpp->x, gpp->y, names[1], names[2]);
    printf("%d %d\n", gcopy.x, gcopy.y);
    printf("%d %d %d\n", gtab[0].v[1], gtab[1].v[2], sum(gtab[1].v, gtab[1].n));
    printf("%d %d %d %g %d\n", gzero, gs.a, gs.p.y, gs.d, garr[3]);

    /* in expressions and as arguments */
    printf("%d\n", sum((int[]){ 1, 2, 3, 4 }, 4));
    printf("%d\n", sum((int[5]){ [4] = 50, [0] = 1 }, 5));
    q = mid((struct P){ 2, 4 }, (struct P){ 6, 8 });
    printf("%d %d\n", q.x, q.y);
    printf("%d\n", area(&(struct P){ 3, 7 }));
    printf("%g\n", dot((double[]){ 1.5, 2 }, (double[2]){ 4, 0.5 }));
    printf("%d\n", (int){ 42 } + (int){ 0 });
    printf("%zu %zu\n", sizeof (int[]){ 1, 2, 3 }, sizeof (struct P){ 0 });

    /* assignment of a whole record, member access, lvalue */
    s = (struct S){ .name = "new", .p.y = 8 };
    printf("%d %d %d %g %s\n", s.a, s.p.x, s.p.y, s.d, s.name);
    (struct P){ 1, 1 }.x = 5;
    p = (int[]){ 0, 0, 0 };
    p[1] = 11;
    printf("%d %d %d\n", p[0], p[1], p[2]);
    printf("%d\n", ((struct P){ 6, 7 }).y);

    /* evaluated afresh each time it is reached; each literal is its own object */
    for (i = 0; i < 3; i++) {
        int *v = (int[]){ i, i * 10 };
        ptrs[i] = v;
        printf("%d %d ", v[0], v[1]);
        v[0] = 99;
    }
    printf("\n");
    counter = 0;
    for (i = 0; i < 3; i++) {
        counter += (struct P){ .x = i }.y;   /* y is zero each time */
    }
    printf("%d %d\n", counter, ptrs[2][0]);

    /* initializers with runtime values */
    i = 5;
    q = (struct P){ i, i * 2 };
    printf("%d %d\n", q.x, q.y);
    p = (int[]){ i, i + 1, i + 10 };
    i++;
    printf("%d %d\n", p[0] + p[1], i);

    /* empty initializers */
    printf("%d %g %d %zu\n", z, dz, (int)strlen(buf), sizeof e);
    {
        struct S t = {};
        struct P arr[3] = { {}, { 1 }, {} };
        printf("%d %d %g %d %d %d\n", t.a, t.p.y, t.d, t.name == 0, arr[1].x, arr[2].y);
    }
    return 0;
}
