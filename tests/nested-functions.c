/* GNU nested functions: the enclosing functions' locals and parameters, two levels deep, siblings,
   recursion, trampolines (a nested function's address, passed to qsort), and a jump out of one
   to a __label__ of the function it is in */
#include <stdio.h>
#include <stdlib.h>

static int apply(int (*f)(int), int x) { return f(x); }

static int outer(int base, int n) {
    int total = 0;
    int scale = 3;
    int add(int x) { total += x * scale; return total; }
    int twice(int x) { return add(add(x) ? x : 0); }
    int deep(int x) {
        int y = x + base;
        int deeper(int z) { return z + y + base + scale; }
        return deeper(x);
    }
    int fact(int k) { return k <= 1 ? 1 : k * fact(k - 1); }
    int i;
    for (i = 0; i < n; i++) {
        add(i);
    }
    twice(2);
    printf("total %d deep %d fact %d", total, deep(5), fact(5));
    printf(" via ptr %d\n", apply(add, 100));
    return total;
}

static int sort_desc(int *a, int n, int bias) {
    int calls = 0;
    int cmp(const void *x, const void *y) {
        calls++;
        return (*(const int *)y + bias) - (*(const int *)x + bias);
    }
    qsort(a, n, sizeof(int), cmp);
    return calls > 0;
}

static int find(const int *a, int n, int want) {
    __label__ found;
    int at = -1;
    void scan(int i) {
        if (i == n) {
            return;
        }
        if (a[i] == want) {
            at = i;
            goto found;     /* out of scan, however deep, back into find */
        }
        scan(i + 1);
    }
    scan(0);
    return -1;
found:
    return at;
}

int main(void) {
    int a[6] = { 3, 1, 4, 1, 5, 9 };
    int k;
    printf("%d\n", outer(10, 4));
    printf("%d:", sort_desc(a, 6, 2));
    for (k = 0; k < 6; k++) printf(" %d", a[k]);
    printf("\n");
    printf("%d\n", find(a, 6, 4));
    return 0;
}
