/* __attribute__((alias)) on functions and objects, global and static targets, weak aliases,
   and a static target nothing else uses (ELF only: Mach-O has no aliases) */
#include <stdio.h>

static int impl(int x) { return x * 3; }
int api(int x) __attribute__((alias("impl")));
int api_weak(int) __attribute__((weak, alias("impl")));
extern int also(int) __attribute__((__alias__("api")));

int table[4] = { 1, 2, 3, 4 };
extern int table_alias[4] __attribute__((alias("table")));
static long counter = 5;
extern long counter_alias __attribute__((alias("counter")));
static double unused_target = 2.5;
extern double used_alias __attribute__((alias("unused_target")));

static int (*fp)(int) = api;

int main(void) {
    table_alias[2] = 30;
    counter_alias++;
    printf("%d %d %d %d\n", api(2), api_weak(3), also(4), fp(5));
    printf("%d %ld %g\n", table[2], counter, used_alias);
    printf("%d %d\n", &table_alias[0] == &table[0], &counter_alias == &counter);
    return 0;
}
