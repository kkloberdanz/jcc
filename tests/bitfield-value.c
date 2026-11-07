/* the value of an assignment, compound assignment, ++ or -- to a bit-field,
   or a comma or ?: expression yielding one, promotes by the field's width,
   as a plain read does: an unsigned:17 field becomes an int, so comparing
   it with a negative int is a signed comparison. Postfix ++ and -- are
   where gcc (by the width) and clang (the declared type) differ; jcc
   follows the platform's compiler. Found by csmith (seed 1794080838). */
#include <stdio.h>

struct s {
    unsigned f : 17;
    unsigned g : 32;
    int h : 3;
    unsigned long long w : 40;
};

struct s x;
int one = 1;

int main(void) {
    int n = -22;
    long long big = -1;
    printf("read %d\n", x.f < n);
    printf("assign %d\n", (x.f = 1) <= n);
    printf("add-assign %d\n", (x.f += 1) <= n);
    printf("or-assign %d\n", (x.f |= 1) <= n);
    printf("shl-assign %d\n", (x.f <<= 1) <= n);
    printf("preinc %d\n", (++x.f) <= n);
    printf("predec %d\n", (--x.f) <= n);
    printf("postinc %d\n", (x.f++) <= n);
    printf("postdec %d\n", (x.f--) <= n);
    printf("comma %d\n", (0, x.f) <= n);
    printf("comma assign %d\n", (0, x.f = 2) <= n);
    printf("cond %d\n", (one ? x.f : x.f) <= n);
    printf("cond assign %d\n", (one ? (x.f = 3) : 0u) <= n);
    printf("unsigned 32 %d\n", (x.g = 1) <= n);
    printf("neg %d\n", -(x.f = 5) < 0);
    printf("not %d\n", ~(x.f = 5) < 0);
    printf("wide %d\n", (x.f = 7) > big);
    printf("truncated %d\n", (int)(x.f = 131073));
    printf("signed %d\n", (int)(x.h = 5));
    printf("signed compare %d\n", (x.h = 3) < 0u);
    printf("sizeof %d %d\n", (int)sizeof(x.f = 1), (int)sizeof(x.f + 0));
    printf("w assign %llx\n", (unsigned long long)((x.w = 0xffffffffffULL) + 1));
    printf("w preinc %llx\n", (unsigned long long)((++x.w) + 0));
    printf("w compound %llx\n", (unsigned long long)((x.w += 0xffffffffffULL) + 1));
    printf("w postinc %llx\n", (unsigned long long)((x.w++) + 0xffffffffffULL));
    return 0;
}
