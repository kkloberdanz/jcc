#include <stdarg.h>
#include "ldouble.h"

long double ld_add(long double a, int b, long double c, double d) {
    return a + b + c + d;
}

struct LDRec ld_rec(long double v, int tag) {
    struct LDRec r;
    r.tag = tag;
    r.v = v;
    r.w = v / 2;
    return r;
}

long double ld_sum_rec(struct LDRec r, long double extra) {
    return r.tag + r.v + r.w + extra;
}

long double ld_vsum(int n, ...) {
    va_list ap;
    long double s = 0;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) {
        if (i % 2 == 0) {
            s += va_arg(ap, long double);
        } else {
            s += va_arg(ap, int);
        }
    }
    va_end(ap);
    return s;
}

long double ld_many(double d0, double d1, double d2, double d3, double d4, double d5,
                    double d6, double d7, double d8, long double l0, int i0, long double l1) {
    return d0 + d1 + d2 + d3 + d4 + d5 + d6 + d7 + d8 + l0 + i0 + l1;
}
