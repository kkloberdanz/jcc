/* __attribute__((aligned(n))) on typedefs (which may also lower an alignment), struct members,
   globals, statics and locals (past the frame's 16 bytes too, initialized or not), and
   _Alignof / __alignof__ / __alignof of types and objects */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct { char c[8]; } V __attribute__ ((aligned (8)));
typedef int I8 __attribute__((aligned(8)));
typedef I8 I8b;
typedef int __attribute__((aligned(16))) I16;
typedef double D4 __attribute__((aligned(4)));
struct T { char a; V v; };
struct U { char a; I8 i; };
struct W { char a; int __attribute__((aligned(8))) b; char c; };
struct X { char a; int b __attribute__((aligned(16))); };
struct Y { char a; D4 d; };
struct Z { char c; struct W w; };
struct big { int x[3]; } __attribute__((aligned(64)));

V v;
char pad;
V w;
char pad2;
int g __attribute__((aligned(32)));
char pad3;
static char sbuf[3] __attribute__((aligned(64)));

static unsigned long m(void *p, unsigned long n) {
    return (unsigned long)p % n;
}

static int locals(int k) {
    char a = 1;
    int loc __attribute__((aligned(32))) = k * 3;
    char b = 2;
    struct big s = { { k, k + 1, k + 2 } };
    char str[5] __attribute__((aligned(32))) = "abcd";
    int arr[4] __attribute__((aligned(64))) = { 1, 2 };
    struct big z;
    I16 l16;
    printf("%lu %lu %lu %lu %lu %lu\n", m(&loc, 32), m(&s, 64), m(str, 32), m(arr, 64), m(&z, 64), m(&l16, 16));
    memset(&z, 0, sizeof(z));
    l16 = 4;
    loc += a + b;
    s.x[1] += loc;
    return loc + s.x[0] + s.x[1] + s.x[2] + (int)strlen(str) + arr[0] + arr[1] + arr[3] + z.x[2] + l16;
}

int main(void) {
    printf("%d %d %d %d %d %d\n", (int)__alignof__(V), (int)sizeof(V), (int)sizeof(struct T),
        (int)_Alignof(I8), (int)__alignof__(I8b), (int)_Alignof(I16));
    printf("%d %d %d %d\n", (int)sizeof(struct U), (int)sizeof(struct W), (int)offsetof(struct W, b), (int)sizeof(struct X));
    printf("%d %d %d %d\n", (int)__alignof__(D4), (int)sizeof(struct Y), (int)offsetof(struct Y, d), (int)sizeof(struct Z));
    printf("%d %d %d %d\n", (int)__alignof(g), (int)__alignof__(sbuf), (int)_Alignof(char), (int)__alignof__(struct big));
    printf("%lu %lu %lu %lu\n", m(&v, 8), m(&w, 8), m(&g, 32), m(sbuf, 64));
    printf("%d %d\n", locals(5), locals(-2));
    return 0;
}
