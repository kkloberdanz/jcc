/* C99 array parameter declarators: qualifiers and static in the brackets (named and abstract), [*] in prototypes; an attribute between ( and * in a type name */
#include <stdio.h>

void fill(int [const 4]);
void fill(int a[static 4]);
void fill(int *const a);
int sum(int n, int [__restrict static n]);
int grid(int r, int c, int a[][*]);
int grid(int r, int c, int a[r][c]);
int (*pick(int))(int [volatile 2]);

void fill(int a[const 4]) {
    int i;
    for (i = 0; i < 4; i++) {
        a[i] = i * i;
    }
}

int sum(int n, int v[__restrict static n]) {
    int s = 0;
    while (n-- > 0) {
        s += v[n];
    }
    return s;
}

int grid(int r, int c, int a[r][c]) {
    return a[r - 1][c - 1] + (int)sizeof a[0] / (int)sizeof a[0][0];
}

static int first(int v[volatile 2]) { return v[0]; }
static int second(int v[2]) { return v[1]; }
int (*pick(int which))(int [volatile 2]) { return which ? second : first; }

static int answer(void) { return 42; }

int main(void) {
    int a[4];
    int m[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };
    int pair[2] = { 7, 8 };
    void *fp = (void *)answer;

    fill(a);
    printf("%d %d %d\n", a[3], sum(4, a), grid(2, 3, m));
    printf("%d %d\n", pick(0)(pair), pick(1)(pair));
    printf("%d %d\n", ((int (__attribute__((noinline)) *)(void))fp)(), ((__attribute__((unused)) int (*)(void))fp)());
    return 0;
}
