/* GNU statement expressions and C99 _Bool */
#include <stdio.h>

#define MAX(a, b) ({ int _a = (a), _b = (b); _a > _b ? _a : _b; })
#define SQUARE(x) ({ typeof_int_t = 0; (x) * (x); })

typedef int typeof_int_t;

static int calls = 0;
static int next(void) { return ++calls; }

struct P { int x, y; };

int main(void) {
    _Bool b = 5, c = 0, d = 0.5, e = (void *)0;
    int i = 3;
    double f = ({ double t = 1.5; t * 2; });
    struct P p = ({ struct P q = { 1, 2 }; q.x += 10; q; });
    int m = MAX(next(), next());
    int n = ({ int j; for (j = 0; j < 4; j++) { if (j == 2) break; } j; });

    b++;
    printf("%d %d %d %d %d %d\n", b, c, d, e, (int)sizeof(_Bool), (int)sizeof b);
    printf("%d %d %g %d %d %d %d\n", (int)(_Bool)i, (int)(_Bool)0.0, f, p.x, p.y, m, n);
    ({ i += 100; });
    if (({ int k = i; k > 50; })) {
        printf("big %d\n", i);
    }
    return b + d + m + n + (_Bool)256;
}
