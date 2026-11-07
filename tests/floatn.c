/* The TS 18661-3 interchange types: _Float32, _Float64 and _Float32x are float and double,
   _Float64x long double; _Float128 can be declared, not computed with. glibc's <math.h> and
   <stdlib.h> declare functions on all of them when the compiler claims gcc 7 or later. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

_Float128 f128_identity(_Float128 x);
_Float128 _Complex f128_conj(_Float128 _Complex z);
typedef __float128 quad;

static _Float32 half32(_Float32 x) {
    return x / 2;
}

static _Float64 sum64(_Float64 a, _Float32x b) {
    return a + b;
}

int main(void) {
    _Float32 f = 3.0f;
    _Float64 d = 0.25;
    _Float64x l = 1.5L;
    double r = d * 4;
    printf("%g %g %Lg\n", (double)half32(f), sum64(d, 2.0), l * 2);
    printf("%d %d %d %d\n", (int)sizeof(_Float32), (int)sizeof(_Float64), (int)sizeof(_Float32x),
        (int)(sizeof(_Float64x) == sizeof(long double)));
    printf("%d %d %d\n", (int)sizeof(_Float128), (int)sizeof(quad), (int)sizeof(_Float128 _Complex));
    printf("%g %d\n", r, HUGE_VAL > 1e300);   /* <math.h> for its declarations; no libm call, so no -lm */
    return 0;
}
