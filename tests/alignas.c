/* _Alignas, and __attribute__((aligned(expr))) with an expression rather than a number */
#include <stdio.h>
#include <stddef.h>
typedef int T __attribute__((aligned(2 * sizeof(int))));
typedef int T2 __attribute__((__aligned__ (__alignof__(long long) * 4), unused));
typedef char Low __attribute__((aligned(1)));
typedef long Lower __attribute__((aligned((2)))) ;
struct __attribute__((aligned(sizeof(void *) * 4))) S { char c; };
struct M { char c; int x __attribute__((aligned(sizeof(long) * 2))); };
struct N { char c; _Alignas(16) int x; _Alignas(double) char d; };
int (__attribute__((aligned(4*2))) *fp)(void);
_Alignas(32) static char buf[3];
int main(void) {
    _Alignas(64) int loc = 3;
    int __attribute__((aligned(8 << 2))) loc2 = 0;
    printf("%zu %zu %zu %zu\n", _Alignof(T), _Alignof(T2), _Alignof(Low), _Alignof(Lower));
    printf("%zu %zu\n", _Alignof(struct S), sizeof(struct S));
    printf("%zu %zu\n", offsetof(struct M, x), sizeof(struct M));
    printf("%zu %zu %zu\n", offsetof(struct N, x), offsetof(struct N, d), sizeof(struct N));
    printf("%d %d %d\n", (int)((size_t)buf % 32), (int)((size_t)&loc % 64), (int)((size_t)&loc2 % 32));
    printf("%zu %zu\n", __alignof__(loc), __alignof__(buf));
    return loc - 3 + loc2;
}
