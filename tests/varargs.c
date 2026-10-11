/*
 * Step 5 test: variadic callees (va_start, va_arg, va_end, va_copy) and
 * the byte swap builtins. Built with cc and kcc by tests/run.sh; the exit
 * status and output must agree.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct Pair { int a; int b; };
struct Big { long v[4]; };

static long sum(int n, ...) {
    va_list ap;
    long total = 0;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) {
        total += va_arg(ap, int);
    }
    va_end(ap);
    return total;
}

/* mixed widths and a pointer; more arguments than any ABI has registers for */
static long mixed(const char *tag, int n, ...) {
    va_list ap;
    long total = strlen(tag);
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) {
        switch (i % 4) {
            case 0: total += va_arg(ap, int); break;
            case 1: total += va_arg(ap, long); break;
            case 2: total += va_arg(ap, unsigned); break;
            default: total += strlen(va_arg(ap, const char *)); break;
        }
    }
    va_end(ap);
    return total;
}

/* seven named parameters put some of the named ones on the stack on arm64 */
static int late(int a, int b, int c, int d, int e, int f, int g, int h, int i, ...) {
    va_list ap;
    int x;
    int y;
    va_start(ap, i);
    x = va_arg(ap, int);
    y = va_arg(ap, int);
    va_end(ap);
    return a + b + c + d + e + f + g + h + i + x * 100 + y * 1000;
}

static void vlog(char *buf, const char *fmt, va_list ap) {
    vsprintf(buf, fmt, ap);
}

static void logf2(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(buf, fmt, ap);
    va_end(ap);
}

/* va_copy: read the same arguments twice */
static int twice(int n, ...) {
    va_list ap;
    va_list bp;
    int s1 = 0;
    int s2 = 0;
    int i;
    va_start(ap, n);
    __builtin_va_copy(bp, ap);    /* va_copy is not in C89 stdarg.h */
    for (i = 0; i < n; i++) {
        s1 += va_arg(ap, int);
    }
    for (i = 0; i < n; i++) {
        s2 += va_arg(bp, int) * 2;
    }
    va_end(bp);
    va_end(ap);
    return s1 + s2;
}

/* a va_list passed down through a pointer */
static int next_int(va_list *app) {
    return va_arg(*app, int);
}

static int indirect(int n, ...) {
    va_list ap;
    int s = 0;
    va_start(ap, n);
    while (n-- > 0) {
        s = s * 10 + next_int(&ap);
    }
    va_end(ap);
    return s;
}

/* structs by value through the ellipsis, small and large */
static long structs(int n, ...) {
    va_list ap;
    long s = 0;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) {
        struct Pair p = va_arg(ap, struct Pair);
        struct Big b = va_arg(ap, struct Big);
        s += p.a * 10 + p.b + b.v[0] + b.v[3] * 1000;
    }
    va_end(ap);
    return s;
}

int main(void) {
    char buf[128];
    struct Pair p;
    struct Big b;
    unsigned short h = 0x1234;
    unsigned w = 0x11223344u;
    unsigned long long q = 0x0102030405060708ull;

    printf("%ld\n", sum(0));
    printf("%ld\n", sum(3, 1, 2, 3));
    printf("%ld\n", sum(10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10));
    printf("%ld\n", mixed("ab", 8, 1, 2L, 3u, "four", -5, -6L, 7u, "eight!"));
    printf("%d\n", late(1, 2, 3, 4, 5, 6, 7, 8, 9, 3, 7));
    logf2(buf, "%s-%d-%ld-%c-%x", "x", 42, -7L, 'q', 255);
    puts(buf);
    printf("%d\n", twice(4, 1, 2, 3, 4));
    printf("%d\n", indirect(5, 1, 2, 3, 4, 5));
    p.a = 4;
    p.b = 2;
    b.v[0] = 7;
    b.v[1] = 0;
    b.v[2] = 0;
    b.v[3] = 3;
    printf("%ld\n", structs(2, p, b, p, b));

    printf("%x %x %llx\n", (unsigned)__builtin_bswap16(h), __builtin_bswap32(w), __builtin_bswap64(q));
    printf("%d\n", (int)__builtin_bswap16((unsigned short)0xff00) == 0xff);
    return (int)sum(2, 20, 22);
}
