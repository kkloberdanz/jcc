/* GNU structs with variable length members, declared in a function: their size and the offsets
   after such a member at run time, arrays of them, copies, offsetof, and passing one through ... */
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>

static int sum_va(int n, ...) {
    struct { char c[n]; } v;
    va_list ap;
    int i, s = 0;
    va_start(ap, n);
    v = va_arg(ap, __typeof__(v));
    va_end(ap);
    for (i = 0; i < n; i++)
        s += v.c[i];
    return s;
}

static long layout(int n) {
    struct S { char tag; int a[n]; short after; double d[n]; int last; } s, t[3];
    int i;
    s.tag = 'x';
    for (i = 0; i < n; i++) {
        s.a[i] = i * 10;
        s.d[i] = i / 2.0;
    }
    s.after = 7;
    s.last = 99;
    t[2] = s;
    t[1].last = 5;
    printf("%d %d %d %g %d %d\n", (int)sizeof(s), (int)sizeof(t), t[2].a[n - 1], t[2].d[n - 1], t[2].after, t[2].last + t[1].last);
    printf("%d %d %d\n", (int)offsetof(struct S, after), (int)offsetof(struct S, d[1]), (int)offsetof(struct S, a[n - 1]));
    return (char *)&s.last - (char *)&s;
}

int main(void) {
    int k = 4;
    struct { char c[k]; } x;
    x.c[0] = 1; x.c[1] = 2; x.c[2] = 3; x.c[3] = 4;
    printf("%ld ", layout(3));
    printf("%ld\n", layout(5));
    printf("%d\n", sum_va(k, x));
    return 0;
}
