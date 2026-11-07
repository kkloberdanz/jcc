/* __builtin_apply_args, __builtin_apply and __builtin_return (x86_64): a call forwarded with the
   arguments as they came, in registers and on the stack, and its result returned as it came */
#include <stdio.h>
static double fd(double a, int b, double c) { return a * b + c; }
static long many(long a, long b, long c, long d, long e, long f, long g, long h) { return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8; }
static double fwd_d(double a, int b, double c) {
    void *r = __builtin_apply((void (*)())fd, __builtin_apply_args(), 0);
    __builtin_return(r);
}
static long fwd_many(long a, long b, long c, long d, long e, long f, long g, long h) {
    void *r = __builtin_apply((void (*)())many, __builtin_apply_args(), 16);
    __builtin_return(r);
}
int main(void) {
    printf("%g %ld\n", fwd_d(1.5, 4, 0.25), fwd_many(1, 2, 3, 4, 5, 6, 7, 8));
    return 0;
}
