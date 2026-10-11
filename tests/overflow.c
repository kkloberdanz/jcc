/* __builtin_{add,sub,mul}_overflow: the result in the pointee's type, 1 when the exact result does not fit */
#include <stdio.h>

static int cases = 0;
static int ok = 0;

/* the call first, then the value: reading it in the same expression would be unspecified order */
#define CHECKV(expr, var, want_v, want_f) do { \
    int f = (expr); \
    cases++; \
    if (f == (want_f) && (var) == (want_v)) ok++; \
    else printf("case %d: %s -> %ld flag %d, wanted %ld flag %d\n", cases, #expr, (long)(var), f, (long)(want_v), (want_f)); \
} while (0)
#define CHECK(expr, want_v, want_f) CHECKV(expr, r, want_v, want_f)

int main(void) {
    long r;
    long big = 0x7fffffffffffffffL;
    long small = (long)0x8000000000000000UL;
    unsigned long ubig = 0xffffffffffffffffUL;
    unsigned long ur;
    int ir;
    unsigned uir;
    signed char cr;
    unsigned char ucr;
    unsigned long mr;

    CHECK(__builtin_add_overflow(1L, 2L, &r), 3, 0);
    CHECK(__builtin_add_overflow(big, 1L, &r), small, 1);
    CHECK(__builtin_add_overflow(small, -1L, &r), big, 1);
    CHECK(__builtin_add_overflow(big, -1L, &r), big - 1, 0);
    CHECK(__builtin_sub_overflow(small, 1L, &r), big, 1);
    CHECK(__builtin_sub_overflow(0L, small, &r), small, 1);
    CHECK(__builtin_sub_overflow(10L, 3L, &r), 7, 0);
    CHECK(__builtin_mul_overflow(big, 2L, &r), -2, 1);
    CHECK(__builtin_mul_overflow(big, -1L, &r), small + 1, 0);
    CHECK(__builtin_mul_overflow(small, -1L, &r), small, 1);
    CHECK(__builtin_mul_overflow(3000000000L, 3000000000L, &r), 9000000000000000000L, 0);
    CHECK(__builtin_mul_overflow(4000000000L, 4000000000L, &r), (long)16000000000000000000UL, 1);
    CHECK(__builtin_mul_overflow(-4000000000L, 4000000000L, &r), -(long)16000000000000000000UL, 1);
    /* an unsigned result from signed operands and the other way round */
    CHECKV(__builtin_add_overflow(-1L, 1L, &ur), ur, 0, 0);
    CHECKV(__builtin_add_overflow(-1L, 0L, &ur), ur, ubig, 1);
    CHECKV(__builtin_mul_overflow(ubig, ubig, &mr), mr, 1, 1);
    CHECKV(__builtin_mul_overflow(ubig, 1UL, &mr), mr, ubig, 0);
    CHECKV(__builtin_add_overflow(ubig, 1UL, &mr), mr, 0, 1);
    CHECKV(__builtin_add_overflow(ubig, 1UL, &r), r, 0, 1);
    CHECKV(__builtin_add_overflow(ubig, 1L, &r), r, 0, 1);    /* ubig is unsigned: 2^64 does not fit a long */
    /* narrow results */
    CHECKV(__builtin_add_overflow(100, 100, &ir), ir, 200, 0);
    CHECKV(__builtin_add_overflow(2000000000, 2000000000, &ir), ir, (int)4000000000U, 1);
    CHECKV(__builtin_mul_overflow(65536, 65536, &uir), uir, 0, 1);
    CHECKV(__builtin_mul_overflow(65536, 65535, &uir), uir, 4294901760U, 0);
    CHECKV(__builtin_add_overflow(100, 27, &cr), cr, 127, 0);
    CHECKV(__builtin_add_overflow(100, 28, &cr), cr, -128, 1);
    CHECKV(__builtin_sub_overflow(0, 1, &ucr), ucr, 255, 1);
    CHECKV(__builtin_mul_overflow(-1, -1, &ucr), ucr, 1, 0);
    printf("%d of %d\n", ok, cases);
    return cases - ok;
}
