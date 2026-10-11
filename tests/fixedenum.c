/*
 * Enum underlying types (`enum : T`, C23 and a clang extension the macOS
 * SDK uses), the bit-field they must not be confused with, zero-length
 * arrays, attributes after an abstract declarator, and __func__.
 */
#include <stdio.h>

typedef unsigned int u32;
typedef enum : u32 { F_A = 1, F_B = 0x80000000u } __attribute__((__flag_enum__)) flags_t;
enum __attribute__((__enum_extensibility__(open))) : unsigned char { TINY = 200 };
enum big : long long { BIG = 1LL << 40, NEG = -1 };
typedef enum small_t : signed char { S_NEG = -5, S_POS = 5 } small_t;

enum color { RED, GREEN, BLUE };
struct bits {
    unsigned a : 2;
    enum color : 3;         /* an unnamed bit-field of a complete enum type */
    enum color c : 3;
    enum big : 7;
    int tail;
};

struct pkt {
    int n;
    long data[0];           /* GNU zero-length array */
};

static int apply(int (*f)(int) __attribute__((unused)), int x) { return f(x); }
static int twice(int x) { return 2 * x; }

static const char *who(void) { return __func__; }
static const char *who2(void) { return __FUNCTION__; }

int main(void) {
    flags_t f = F_B;
    enum big b = BIG;
    small_t s = S_NEG;
    struct bits bf;
    struct pkt p;

    printf("%d %d %d %d %d\n", (int)sizeof(flags_t), (int)sizeof(TINY), (int)sizeof(enum big),
           (int)sizeof(small_t), (int)sizeof(S_NEG));
    printf("%u %lld %lld %d %d %d\n", f, b, (long long)NEG, s, TINY, (int)(f > F_A));
    printf("%d %d\n", (int)sizeof(S_NEG + 1), (int)sizeof(BIG + 1));
    bf.a = 3; bf.c = BLUE; bf.tail = -1;
    printf("%d %u %d %d\n", (int)sizeof(struct bits), bf.a, bf.c == BLUE, bf.tail);
    p.n = 4;
    printf("%d %d\n", (int)sizeof(struct pkt), p.n);
    printf("%d %s %s %s\n", apply(twice, 21), who(), who2(), __func__);
    return s < 0 ? 0 : 1;
}
