/* asm labels: a declaration's assembler name given after its declarator (`__asm__("name")`),
   on a library function, a definition and an object, called and taken the address of */
#include <stdio.h>

extern int print_line(const char *) __asm__("puts");
static int twice(int) __asm__("jcc_doubled");
int counter __asm__("renamed_counter") = 3;
extern int counter_again __asm__("renamed_counter");

static int twice(int x) {
    return 2 * x;
}

int main(void) {
    int (*f)(int) = twice;
    print_line("via a label");
    printf("%d %d %d\n", twice(4), f(5), counter + counter_again);
    return 0;
}
