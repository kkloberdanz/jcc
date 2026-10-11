/* Bit counting builtins, the GCC atomic load/store and barrier, and the
   fallthrough attribute as a statement. */
#include <stdio.h>

static int flag;
static long big;
static void *ptr;

static int classify(int n) {
    int r = 0;
    switch (n) {
        case 0:
            r += 1;
            __attribute__((fallthrough));
        case 1:
            r += 10;
            break;
        default:
            r = -1;
    }
    return r;
}

int main(void) {
    unsigned long long x = 0x0000100000000000ULL;
    unsigned u = 0x00f0;
    unsigned long l = 1UL << 40;
    int i;

    printf("%d %d %d %d %d %d\n", __builtin_clzll(x), __builtin_clz(u), __builtin_clzl(l),
           __builtin_ctzll(x), __builtin_ctz(u), __builtin_ctzl(l));
    for (i = 1; i < 32; i += 7) {
        printf("%d:%d:%d ", i, __builtin_clz((unsigned)i), __builtin_ctz((unsigned)i << 3));
    }
    printf("\n");
    __atomic_store_n(&flag, 7, __ATOMIC_RELAXED);
    __atomic_store_n(&big, 123456789012L, __ATOMIC_SEQ_CST);
    __atomic_store_n(&ptr, &flag, __ATOMIC_RELEASE);
    __sync_synchronize();
    printf("%d %ld %d %d\n", __atomic_load_n(&flag, __ATOMIC_RELAXED),
           __atomic_load_n(&big, __ATOMIC_ACQUIRE), __atomic_load_n(&ptr, __ATOMIC_SEQ_CST) == (void *)&flag,
           __atomic_load_n(&flag, __ATOMIC_RELAXED) + 1);
    printf("%d %d %d\n", classify(0), classify(1), classify(2));
    return 0;
}
