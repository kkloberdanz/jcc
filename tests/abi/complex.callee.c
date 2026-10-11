#include <stdarg.h>
#include "complex.h"

double _Complex c_add(double _Complex a, double _Complex b, double k) {
    return (a + b) * k;
}

float _Complex c_mulf(float _Complex a, float _Complex b) {
    return a * b;
}

long double _Complex c_divl(long double _Complex a, long double _Complex b) {
    return a / b;
}

struct CRec c_rec(int tag, float _Complex f, double _Complex d) {
    struct CRec r;
    r.tag = tag;
    r.f = f;
    r.d = d;
    return r;
}

double c_sum_rec(struct CRec r) {
    return r.tag + __real__ r.f + __imag__ r.f + __real__ r.d + __imag__ r.d;
}

double _Complex c_vsum(int n, ...) {
    va_list ap;
    double _Complex s = 0;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) {
        if (i % 3 == 0) {
            s += va_arg(ap, double _Complex);
        } else if (i % 3 == 1) {
            s += va_arg(ap, float _Complex);
        } else {
            s += va_arg(ap, long double _Complex);
        }
    }
    va_end(ap);
    return caller_twice(s);
}

double _Complex c_many(double d0, double d1, double d2, double d3, double d4, double d5, double d6,
                       double _Complex a, float _Complex b, int i, long double _Complex c) {
    return d0 + d1 + d2 + d3 + d4 + d5 + d6 + a + b + i + c;
}
