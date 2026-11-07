/* C99 variable length arrays: objects, sizeof, several dimensions, pointers to VLAs, parameters, typedefs, loops that reuse the stack */
#include <stdio.h>
#include <string.h>

static int calls;
static int next(int n) { calls++; return n; }

static long sum(int n, int a[n]) {
    long s = 0;
    int i;
    for (i = 0; i < n; i++) {
        s += a[i];
    }
    return s;
}

/* a parameter adjusted to a pointer to a VLA: rows of m */
static long trace(int n, int m, double a[n][m]) {
    long t = 0;
    int i;
    for (i = 0; i < n && i < m; i++) {
        t += (long)a[i][i];
    }
    return t + (long)sizeof(a[0]) + (long)(&a[1][0] - &a[0][0]);
}

static void fill(int n, int m, int (*p)[m]) {
    int i, j;
    for (i = 0; i < n; i++) {
        for (j = 0; j < m; j++) {
            p[i][j] = i * 100 + j;
        }
    }
}

static int depth(int n) {
    char buf[n + 1];
    memset(buf, 'a' + n % 26, sizeof buf);
    buf[n] = 0;
    if (n == 0) {
        return 0;
    }
    return depth(n - 1) + (int)strlen(buf);
}

int main(void) {
    int n = 5, m = 3, i, j, k;
    long total = 0;

    {
        int a[n];
        for (i = 0; i < n; i++) {
            a[i] = i * i;
        }
        printf("%zu %ld\n", sizeof a, sum(n, a));
        printf("%zu %zu\n", sizeof(int[n]), sizeof(int[n][m]) / sizeof(int));
    }
    {
        double grid[n][m];
        int g2[m][n];
        int (*row)[n] = g2;
        for (i = 0; i < n; i++) {
            for (j = 0; j < m; j++) {
                grid[i][j] = i + j * 10;
            }
        }
        printf("%zu %zu %zu\n", sizeof grid, sizeof grid[0], sizeof grid[0][0]);
        printf("%ld\n", trace(n, m, grid));
        fill(m, n, g2);
        printf("%d %d %d\n", g2[2][4], row[1][3], (*(row + 2))[1]);
        row++;
        printf("%d %td\n", row[0][0], row - (int (*)[n])g2);
    }
    {
        typedef int vec[n];
        vec v1, v2;
        n = 100;            /* the typedef keeps the size it had */
        printf("%zu %zu\n", sizeof(vec), sizeof v2);
        v1[0] = 1;
        v2[4] = 2;
        printf("%d %d\n", v1[0], v2[4]);
        n = 5;
    }
    /* the size expression is evaluated once, where the declarator is reached */
    {
        int b[next(4)];
        printf("%zu %d\n", sizeof b, calls);
    }
    /* a loop that would exhaust the stack if each iteration's array stayed */
    for (k = 0; k < 200000; k++) {
        int big[1000 + k % 7];
        big[0] = k;
        big[999] = 1;
        total += big[0] + big[999];
        if (k % 50000 == 0) {
            continue;
        }
    }
    printf("%ld\n", total);
    /* nested loops, a goto backwards past a declaration */
    k = 0;
again:
    {
        char tmp[k + 10];
        tmp[k] = (char)k;
        if (++k < 100000) {
            goto again;
        }
    }
    printf("%d %d\n", k, depth(30));
    return 0;
}
