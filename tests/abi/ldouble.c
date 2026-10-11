#include <stdio.h>
#include "ldouble.h"

/* the callee calls back into this side too */
long double caller_check(long double seed) {
    return seed * 2 + 0.5L;
}

int main(void) {
    struct LDRec r = ld_rec(3.25L, 9);
    printf("%Lg\n", ld_add(1.5L, 2, 0.25L, 4.5));
    printf("%d %Lg %g\n", r.tag, r.v, r.w);
    printf("%Lg\n", ld_sum_rec(r, 100));
    printf("%Lg\n", ld_vsum(4, 1.5L, 2, 0.25L, 3));
    printf("%Lg\n", ld_many(1, 2, 3, 4, 5, 6, 7, 8, 9, 0.5L, 10, 0.25L));
    printf("%Lg\n", caller_check(ld_add(1, 1, 1, 1)));
    return 0;
}
