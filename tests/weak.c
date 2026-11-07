/* __attribute__((weak)): weak definitions, and weak references left undefined (their address is null) */
#include <stdio.h>
extern void missing(void) __attribute__((weak));
extern int missing_var __attribute__((weak));
int __attribute__((weak)) wvar = 1;
__attribute__((weak)) int hook(int x) { return x + 1; }
int over(int x) __attribute__((weak));
int over(int x) { return x * 10; }
int main(void) {
    int (*p)(int) = hook;
    printf("%d %d %d %d %d %d\n", missing == 0, &missing_var == 0, wvar, hook(1), over(2), p(5));
    if (missing) missing();
    return 0;
}
