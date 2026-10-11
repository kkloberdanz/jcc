#include <stdio.h>
#include "narrow.h"
long many(char a, short b, int c, long d, unsigned char e, unsigned short f,
          int g, long h, char i, short j, unsigned char k, unsigned short l,
          int m, char n) {
    printf("callee: %d %d %d %ld %u %u %d %ld %d %d %u %u %d %d\n", a, b, c, d, e, f, g, h, i, j, k, l, m, n);
    return a + b + c + d + e + f + g + h + i + j + k + l + m + n;
}
double manyf(double a, double b, double c, double d, double e, double f, double g, double h,
             float i, double j, float k, char l, short m) {
    printf("callee: %g %g %g %g %g %g %g %g %g %g %g %d %d\n", a, b, c, d, e, f, g, h, i, j, k, l, m);
    return a + b + c + d + e + f + g + h + i + j + k + l + m;
}
long callee_calls_back(void) {
    long a = callback(-1, -2, -3, -4, 255, 65535, 7, 8, -9, -10, 11, 12, -13, -14);
    long b = callback('x', 300, 70000, 1L << 40, 200, 40000, -1, -1, 'y', -300, 1, 2, 3, 4);
    return a + b;
}
