#include <stdio.h>
#include "varargs.h"
long vsum(int n, ...) {
    va_list ap; long s = 0; int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) s += va_arg(ap, long);
    va_end(ap);
    return s;
}
double vmix(const char *fmt, ...) {
    va_list ap; double s = 0;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        switch (*fmt) {
        case 'i': { int v = va_arg(ap, int); printf("callee: i %d\n", v); s += v; break; }
        case 'l': { long v = va_arg(ap, long); printf("callee: l %ld\n", v); s += v; break; }
        case 'd': { double v = va_arg(ap, double); printf("callee: d %g\n", v); s += v; break; }
        case 'p': { const char *v = va_arg(ap, const char *); printf("callee: p %s\n", v); s += v[0]; break; }
        case 'v': { int n = va_arg(ap, int); s += caller_vpass(n, ap); break; } /* must be last: ap may or may not advance */
        }
    }
    va_end(ap);
    return s;
}
long vpass(int n, va_list ap) {
    long s = 0; int i;
    for (i = 0; i < n; i++) s += va_arg(ap, long);
    return s;
}
long callee_calls_caller(void) {
    return (long)vmix("ildpv", 1, 2L, 3.5, "q", 3, 10L, 20L, 30L);
}
