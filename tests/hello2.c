#include <stdio.h>

int foo(void) {
    int x = 15;
    int y = 3;
    return x + y;
}

int main(void) {
    int z = foo();
    printf("hello: %d\n", z);

    return 0;
}
