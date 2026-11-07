/* C11 _Static_assert (file scope, block scope, struct member; C23's form without a message) and _Noreturn */
#include <stdio.h>
#include <stdlib.h>

_Static_assert(sizeof(int) == 4, "int is 32 bits");
_Static_assert(_Generic(1L, long: 1, default: 0), "1L is a long");

struct S {
    int a;
    _Static_assert(sizeof(char) == 1, "char is a byte");
    char b;
};

static _Noreturn void die(int code) {
    exit(code);
}

_Noreturn static void die2(void);

static void die2(void) {
    exit(0);
}

int main(void) {
    _Static_assert(sizeof(struct S) == 8, "two members, padded");
    int x = 3;
    _Static_assert(1);
    printf("%d %zu\n", x, sizeof(struct S));
    if (x == 4) {
        die(1);
    }
    die2();
}
