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

/* LDBL_MIN / 4 and LDBL_MAX, built at run time from the integer macros by exact
   powers of two: jcc folds floating constants in the host's long double, so
   when cross compiling from a host whose long double is a double (arm64 macOS)
   for an x87 target, LDBL_MIN and LDBL_MAX as constants come out 0 and inf.
   That is a known limitation; this test is about the builtins. */
static volatile long double half = 0.5L, two = 2.0L;

static long double ldbl_subnormal(void) {
    long double x = 1.0L;
    int i;
    for (i = 0; i < 3 - LDBL_MIN_EXP; i++) {
        x *= half;
    }
    return x;
}

static long double ldbl_max(void) {
    long double eps = 1.0L, x;
    int i;
    for (i = 0; i < LDBL_MANT_DIG - 1; i++) {
        eps *= half;
    }
    x = 2.0L - eps;
    for (i = 0; i < LDBL_MAX_EXP - 1; i++) {
        x *= two;
    }
    return x;
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
        show_l(names[i], i == 7 ? ldbl_subnormal() : (long double)vals[i]);
    }
    show_f("flt-denorm-as-dbl", (float)(FLT_MIN / 4));
    show_d("dbl-max", DBL_MAX);
    show_l("ldbl-max", ldbl_max());
    cmp(1, 2); cmp(2, 1); cmp(1, 1); cmp(nan, 1); cmp(1, nan);
    printf("%d %d %d %d\n", __builtin_mul_overflow_p(4, 200, (unsigned char)0), __builtin_mul_overflow_p(4, 20, (unsigned char)0),
           __builtin_add_overflow_p(0x7fffffff, 1, 0), __builtin_sub_overflow_p(0, 1, 0u));
    return 0;
}
