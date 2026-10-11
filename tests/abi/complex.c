#include <stdio.h>
#include "complex.h"

double _Complex caller_twice(double _Complex z) {
    return z * 2;
}

static void show(const char *what, double _Complex z) {
    printf("%s %g %g\n", what, __real__ z, __imag__ z);
}

int main(void) {
    double _Complex a = 1.0 + 2.0i;
    float _Complex f = 1.5f + 0.5fi;
    long double _Complex l = 2.0L + 1.0iL;
    struct CRec r = c_rec(5, f, a);
    show("add", c_add(a, 3.0 - 1.0i, 2));
    show("mulf", c_mulf(f, f));
    show("divl", c_divl(l, 1.0L + 1.0iL));
    printf("rec %d %g %g %g %g\n", r.tag, (double)__real__ r.f, (double)__imag__ r.f, __real__ r.d, __imag__ r.d);
    printf("sum rec %g\n", c_sum_rec(r));
    show("vsum", c_vsum(6, a, f, l, a, f, l));
    show("many", c_many(1, 2, 3, 4, 5, 6, 7, a, f, 10, l));
    return 0;
}
