/* C99 designated initializers: members, indexes, paths, and continuing after one */
#include <stdio.h>

struct P { int x, y; };
struct S { int a; struct P p; int arr[4]; const char *name; };
union U { int i; char c[4]; double d; };

static struct S gs = { .name = "gs", .arr[1] = 10, 11, .p.y = 5, .a = 1 };
static int table[8] = { [6] = 60, 70, [1] = 10, [0] = 1 };
static struct P ps[3] = { [2].y = 22, [0] = { .y = 2, .x = 1 }, [1].x = 11 };
static union U gu = { .c = { 'a', 'b', 'c', 0 } };
static struct S overridden = { .a = 1, .a = 2 };
static struct { int k; struct { int m, n; } in; } nested = { .in.n = 3, .k = 9 };
static int sparse[20] = { [19] = 1 };

enum { K = 3 };

int main(void) {
    struct S ls = { .p = { 7, 8 }, .arr = { [K] = 33 }, .name = "ls" };
    int i;
    int la[5] = { [1] = 2, [3] = 4 };
    union U lu = { .d = 1.5 };

    printf("%d %d %d %d %d %d %d %s\n", gs.a, gs.p.x, gs.p.y, gs.arr[0], gs.arr[1], gs.arr[2], gs.arr[3], gs.name);
    for (i = 0; i < 8; i++) {
        printf("%d ", table[i]);
    }
    printf("\n");
    for (i = 0; i < 3; i++) {
        printf("(%d,%d) ", ps[i].x, ps[i].y);
    }
    printf("\n%s %d %d %d %d\n", gu.c, overridden.a, nested.k, nested.in.m, nested.in.n);
    printf("%d %d %d %d %s\n", ls.a, ls.p.x, ls.p.y, ls.arr[3], ls.name);
    for (i = 0; i < 5; i++) {
        printf("%d ", la[i]);
    }
    printf("%d %g %d\n", (int)sizeof(sparse) / (int)sizeof(sparse[0]), lu.d, sparse[19]);
    return gs.arr[2] + table[7] + ps[1].x;
}
