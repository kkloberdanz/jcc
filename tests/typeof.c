/* GNU __typeof__ (of expressions, which are not evaluated, and of type names) and __auto_type */
#include <stdio.h>
typedef int T;
struct P { int x, y; };
static int calls;
static int f(void) { return ++calls; }
#define SWAP(a, b) do { __typeof__(a) _t = (a); (a) = (b); (b) = _t; } while (0)
#define MAX(a, b) ({ __auto_type _a = (a); __auto_type _b = (b); _a > _b ? _a : _b; })
int main(void) {
    int i = 3, j = 4;
    double d = 2.5;
    __typeof__(d) e = 1.25;
    __typeof__(int *) p = &i;
    __typeof__(T) t = 7;
    const __typeof(i) ci = 9;
    __typeof__(f()) never = 0;
    int arr[5];
    __typeof__(arr) arr2;
    struct P sp = { 1, 2 };
    __auto_type q = sp;
    __auto_type ptr = arr;
    __auto_type c = 'x';
    const __auto_type k = d * 2;
    __typeof__(__typeof__(char) *) cp = "hi";
    __typeof__(sp.x + 1L) lv = 1;
    SWAP(i, j);
    printf("%d %d %g %d %d %d %d %zu %d %d %zu %g %s %zu %zu %d\n", i, j, e, *p, t, ci, never, sizeof(arr2), q.y,
           ptr == arr, sizeof(c), k, cp, sizeof(lv), sizeof(__typeof__(c)), MAX(i, f()));
    printf("%d\n", calls);
    return 0;
}
