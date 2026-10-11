#include <stdio.h>
#include "varargs.h"
long caller_vpass(int n, va_list ap) {
    long s = 0; int i;
    for (i = 0; i < n; i++) { long v = va_arg(ap, long); printf("caller: v %ld\n", v); s += v; }
    return s;
}
static long through(int n, ...) {
    va_list ap; long r;
    va_start(ap, n);
    r = vpass(n, ap);
    va_end(ap);
    return r;
}
int main(void) {
    printf("%ld\n", vsum(0));
    printf("%ld\n", vsum(3, 1L, -2L, 300000000000L));
    printf("%ld\n", vsum(10, 1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L, 9L, 10L));
    printf("%g\n", vmix("idlpdiiiiiiiiid", 1, 2.5, -3L, "hello", 4.25, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14.75));
    printf("%ld\n", through(4, 10L, 20L, 30L, 40L));
    printf("%ld\n", callee_calls_caller());
    return (int)vsum(2, 20L, 22L);
}
