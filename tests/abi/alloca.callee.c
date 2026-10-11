#include <alloca.h>
#include <string.h>
#include "alloca.h"

void fill(char *p, int n, char c) {
    memset(p, c, n);
}

/* allocates here, fills through the other compiler's fill */
int callee_sums(int n) {
    char *p = alloca(n);
    int i;
    int s = 0;
    fill(p, n, 3);
    for (i = 0; i < n; i++) {
        s += p[i];
    }
    return s;
}
