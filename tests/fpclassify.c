/* the GNU classification builtins behind <math.h>'s isnan, isinf, fpclassify, signbit, isgreater
   and the rest, for float, double and long double, and __builtin_*_overflow_p */
#include <stdio.h>
#include <float.h>

#define CLASSIFY(x) __builtin_fpclassify(0, 1, 2, 3, 4, x)

static volatile double zero = 0.0;

static void show_d(const char *what, double x) {
    printf("%-10s d: nan %d inf %d sign %d fin %d norm %d cls %d sb %d\n", what,
           __builtin_isnan(x) != 0, __builtin_isinf(x) != 0, __builtin_isinf_sign(x), __builtin_isfinite(x) != 0,
           __builtin_isnormal(x) != 0, CLASSIFY(x), __builtin_signbit(x) != 0);
}

static void show_f(const char *what, float x) {
    printf("%-10s f: nan %d inf %d sign %d fin %d norm %d cls %d sb %d %d %d\n", what,
           __builtin_isnan(x) != 0, __builtin_isinff(x) != 0, __builtin_isinf_sign(x), __builtin_isfinite(x) != 0,
           __builtin_isnormal(x) != 0, CLASSIFY(x), __builtin_signbit(x) != 0, __builtin_signbitf(x) != 0,
           __builtin_isnanf(x) != 0);
}

static void show_l(const char *what, long double x) {
    printf("%-10s l: nan %d inf %d sign %d fin %d norm %d cls %d sb %d %d\n", what,
           __builtin_isnan(x) != 0, __builtin_isinfl(x) != 0, __builtin_isinf_sign(x), __builtin_isfinite(x) != 0,
           __builtin_isnormal(x) != 0, CLASSIFY(x), __builtin_signbit(x) != 0, __builtin_isnanl(x) != 0);
}

static void cmp(double a, double b) {
    printf("%d %d %d %d %d %d\n", __builtin_isgreater(a, b), __builtin_isgreaterequal(a, b), __builtin_isless(a, b),
           __builtin_islessequal(a, b), __builtin_islessgreater(a, b), __builtin_isunordered(a, b));
}

int main(void) {
    double inf = 1.0 / zero, nan = zero / zero;
    double vals[8];
    const char *names[8] = { "zero", "-zero", "one", "-two", "inf", "-inf", "nan", "subnormal" };
    int i;
    vals[0] = 0.0; vals[1] = -zero; vals[2] = 1.0; vals[3] = -2.0;
    vals[4] = inf; vals[5] = -inf; vals[6] = nan; vals[7] = DBL_MIN / 4;
    for (i = 0; i < 8; i++) {
        show_d(names[i], vals[i]);
        show_f(names[i], i == 7 ? FLT_MIN / 4 : (float)vals[i]);
        show_l(names[i], i == 7 ? LDBL_MIN / 4 : (long double)vals[i]);
    }
    show_f("flt-denorm-as-dbl", (float)(FLT_MIN / 4));
    show_d("dbl-max", DBL_MAX);
    show_l("ldbl-max", LDBL_MAX);
    cmp(1, 2); cmp(2, 1); cmp(1, 1); cmp(nan, 1); cmp(1, nan);
    printf("%d %d %d %d\n", __builtin_mul_overflow_p(4, 200, (unsigned char)0), __builtin_mul_overflow_p(4, 20, (unsigned char)0),
           __builtin_add_overflow_p(0x7fffffff, 1, 0), __builtin_sub_overflow_p(0, 1, 0u));
    return 0;
}
