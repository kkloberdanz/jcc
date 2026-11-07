/* __builtin_setjmp and __builtin_longjmp: back from deeper frames (with callee-saved registers in
   use in them), past alloca, more than once */
#include <stdio.h>

static void *buf[5];
static int depth_reached;

static int __attribute__((noinline)) dive(int n, int x) {
    int a = x * 3, b = x + 7, c = x ^ 5;
    if (n == 0) {
        depth_reached = a + b + c;
        __builtin_longjmp(buf, 1);
    }
    return dive(n - 1, a + b) + c;
}

int main(void) {
    volatile int tries = 0;
    char *p = __builtin_alloca(16);
    p[0] = 'k';
    if (__builtin_setjmp(buf)) {
        tries++;
        printf("back %d %d %c\n", tries, depth_reached, p[0]);
        if (tries < 3) {
            __builtin_alloca(64);
            dive(tries * 4, tries);
        }
        return 0;
    }
    dive(10, 2);
    printf("not reached\n");
    return 1;
}
