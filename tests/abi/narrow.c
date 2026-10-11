#include <stdio.h>
#include "narrow.h"
long callback(char a, short b, int c, long d, unsigned char e, unsigned short f,
              int g, long h, char i, short j, unsigned char k, unsigned short l,
              int m, char n) {
    printf("caller: %d %d %d %ld %u %u %d %ld %d %d %u %u %d %d\n", a, b, c, d, e, f, g, h, i, j, k, l, m, n);
    return a + b + c + d + e + f + g + h + i + j + k + l + m + n;
}
int main(void) {
    long r;
    char c = -5; short s = -600; unsigned char uc = 250; unsigned short us = 65000;
    r = many(-1, -2, -3, -4, 255, 65535, 7, 8, -9, -10, 11, 12, -13, -14);
    printf("%ld\n", r);
    r = many(c, s, 3, 4, uc, us, 7, 8, c, s, uc, us, 13, c);
    printf("%ld\n", r);
    printf("%g\n", manyf(1, 2, 3, 4, 5, 6, 7, 8, 9.5f, 10.25, 11.75f, -12, -13));
    printf("%ld\n", callee_calls_back());
    return (int)(r & 0x7f);
}
