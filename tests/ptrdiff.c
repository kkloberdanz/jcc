/* pointer differences scale by the element size, for every size */
#include <stdio.h>

struct Big { double d; long l; char c; };   /* 24 bytes */
struct S16 { long a, b; };

static long diff_int(int *a, int *b) { return a - b; }
static long diff_big(struct Big *a, struct Big *b) { return a - b; }

int main(void) {
    char cs[10];
    short ss[10];
    int is[10];
    long ls[10];
    struct S16 s16[10];
    struct Big bigs[10];
    double ds[10];
    int *ip = is + 7;
    long n;

    printf("%ld %ld %ld %ld %ld %ld %ld\n",
           (long)(&cs[9] - &cs[2]), (long)(&ss[9] - &ss[2]), (long)(&is[9] - &is[2]), (long)(&ls[9] - &ls[2]),
           (long)(&s16[9] - &s16[2]), (long)(&bigs[9] - &bigs[2]), (long)(&ds[9] - &ds[2]));
    printf("%ld %ld %ld %ld\n", (long)(is - ip), diff_int(is + 3, is), diff_big(bigs + 5, bigs + 1), diff_big(bigs, bigs + 4));
    n = (ip - is) * 2 + (&bigs[3] - bigs);
    if (ip - is <= 6) {
        printf("wrong\n");
    }
    if (bigs + 9 - bigs <= 8) {
        printf("wrong2\n");
    }
    printf("%d %d\n", (int)(ip - is) == 7, (int)sizeof(struct Big));
    return (int)n;
}
