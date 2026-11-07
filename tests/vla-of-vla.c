/* an array of variable length arrays with a constant outer dimension (`int b[2][n]`) is sized,
   allocated and indexed at run time like any other variable length array */
#include <stdio.h>

static int fill(int n) {
    int a[n];
    int b[2][n];
    int c[n][2];
    int d[3][2][n];
    int i, j, k, sum = 0;
    for (i = 0; i < 2; i++)
        for (j = 0; j < n; j++)
            b[i][j] = 10 * i + j;
    for (i = 0; i < 3; i++)
        for (j = 0; j < 2; j++)
            for (k = 0; k < n; k++)
                d[i][j][k] = i * 100 + j * 10 + k;
    for (j = 0; j < n; j++)
        sum += b[1][j] + d[2][1][j];
    a[0] = c[0][0] = 0;
    printf("%d %d %d %d %d %d\n", (int)sizeof(a), (int)sizeof(b), (int)sizeof(c), (int)sizeof(d), (int)sizeof(d[1]), sum);
    return sum;
}

int main(void) {
    int n = 4;
    int x[2][n];
    x[1][3] = 7;
    n++;
    printf("%d %d\n", (int)sizeof(x), x[1][3]);
    fill(3);
    fill(5);
    return 0;
}
