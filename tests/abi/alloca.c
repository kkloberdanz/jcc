#include <alloca.h>
#include <stdio.h>
#include <string.h>
#include "alloca.h"

int caller_sums(int n) {
    char *p = alloca(n);
    int i;
    int s = 0;
    fill(p, n, 5);
    for (i = 0; i < n; i++) {
        s += p[i];
    }
    return s;
}

int main(void) {
    int n;
    for (n = 1; n < 3000; n = n * 3 + 1) {
        printf("%d %d %d\n", n, caller_sums(n), callee_sums(n));
    }
    return 0;
}
