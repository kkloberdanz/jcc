/* arithmetic on bit-fields wider than an int: gcc does it at the field's width
   (`unsigned long long:40`), clang in the declared type; jcc follows the
   platform's compiler, which is what the host's cc is here */
#include <stdio.h>

struct s {
    unsigned long long u33 : 33;
    unsigned long long u40 : 40;
    long long s35 : 35;
    long long s40 : 40;
    unsigned long ul48 : 48;
    unsigned long long u32 : 32;
    unsigned long long u20 : 20;
};

static unsigned long long show(const char *what, unsigned long long v) {
    printf("%s %llx\n", what, v);
    return v;
}

int main(void) {
    struct s x;
    int m = -1;
    unsigned u = 0xffffffffu;
    x.u33 = 0x100;
    x.u40 = 0x100;
    x.s35 = -2;
    x.s40 = 0x7fffffffffLL;
    x.ul48 = 0xffffffffffffUL;
    x.u32 = 0xffffffffu;
    x.u20 = 0xfffff;
    show("shl", x.u40 << 32);
    show("rotate", (x.u40 << 8) + (x.u40 >> 32));
    show("mul", x.u33 * x.u40 * 0x100000000ULL);
    show("add", x.u33 + x.u40);
    show("sub", x.u33 - x.u40);
    show("neg", -x.u40);
    show("not", ~x.u40);
    show("mixed int", x.u40 + m);
    show("mixed unsigned", x.u40 * u);
    show("signed", x.s35 * 0x10);
    show("signed overflow", x.s40 + x.s40 / 2);
    show("signed vs unsigned", x.s35 + x.u33);
    show("compare", x.u40 == (unsigned long long)m);
    show("compare int", x.s35 < m);
    show("wide ulong", x.ul48 + 1);
    show("u32", x.u32 + 1);
    show("u20", x.u20 << 20);
    show("cond", 1 ? x.u33 + x.u33 : 0);
    show("cast", (unsigned long long)x.u40 << 32);
    show("sizeof", sizeof(x.u40 + 1));
    return 0;
}
