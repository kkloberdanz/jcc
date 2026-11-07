/* initializers: a string literal filling a char array whose braces are elided, GNU range designators, designators into anonymous members, several designators for one union */
#include <stdio.h>

typedef unsigned char u8;
struct S { u8 a, b; u8 c[2]; };
struct T { u8 s[8]; u8 a; };
struct U { u8 a; struct S s; u8 b; struct T t; };
struct V { struct S s; struct T t; u8 a; };
struct N { int n; char name[6]; char tag[3]; };
union UV { struct { u8 a, b; }; struct S s; };
struct A { int k; union { int i; struct { short lo, hi; }; }; int z; };

static void dump(const char *name, const void *p, size_t size) {
    const u8 *b = p;
    printf("%s:", name);
    while (size--) {
        printf(" %x", *b++);
    }
    printf("\n");
}
#define DUMP(x) dump(#x, &x, sizeof(x))

/* the string fills t.s, then 43 goes to t.a */
static struct U gu = { 3, 5, 6, 7, 8, 4, "huhu", 43 };
static struct V gv = { { 3, 4, { 5, 6 } }, "haha", 45, 46 };
static struct N gn[] = { 1, "one", "x", 2, "two", "yy" };
static char gwords[2][4] = { "ab", "cde" };

static int gr[10] = { [1 ... 4] = 7, [3 ... 5] = 9, [8] = 1 };
static struct S gsr[4] = { [0 ... 3] = { 1, 2, { 3, 4 } }, [2].b = 9 };
static int grows[3][2] = { [0 ... 2] = { 5, 6 }, [1][1] = 0 };
static int gopen[] = { [2 ... 4] = 3 };     /* the range sets the length: 5 */

static void one(void) { printf("one\n"); }
static void other(void) { printf("other\n"); }
static void (*const table[4])(void) = { [0 ... 3] = other, [1] = one };

static union UV guv = { .b = 8, .a = 7 };
static union UV guv2 = { { .b = 7, .a = 8 } };
static struct A ga = { .hi = 2, .lo = 1, .z = 3 };
static struct A ga2 = { 1, .i = 5, 9 };

int main(void) {
    int i;
    int x = 4;
    struct U lu = { 3, 5, 6, 7, 8, 4, "huhu", 43 };
    struct V lv = { { 3, 4, { 5, 6 } }, "haha", 45, 46 };
    struct N ln[] = { 1, "one", "x", 2, "two", "yy" };
    int lr[8] = { [0 ... 7] = x, [2 ... 3] = x + 1 };
    struct S lsr[3] = { [0 ... 2] = { 1, 2, { 3, 4 } } };
    union UV luv = { .b = 8, .a = 7 };
    struct A la = { .hi = 2, .lo = 1, .z = 3 };

    DUMP(gu);
    DUMP(lu);
    DUMP(gv);
    DUMP(lv);
    printf("%d %s %s %d %s %s %zu\n", gn[0].n, gn[0].name, gn[0].tag, gn[1].n, gn[1].name, gn[1].tag, sizeof gn / sizeof gn[0]);
    printf("%d %s %s %d %s %s\n", ln[0].n, ln[0].name, ln[0].tag, ln[1].n, ln[1].name, ln[1].tag);
    printf("%s %s\n", gwords[0], gwords[1]);

    for (i = 0; i < 10; i++) {
        printf(" %d", gr[i]);
    }
    printf("\n");
    DUMP(gsr);
    for (i = 0; i < 6; i++) {
        printf(" %d", grows[i / 2][i % 2]);
    }
    printf(" | %zu\n", sizeof gopen / sizeof gopen[0]);
    for (i = 0; i < 4; i++) {
        table[i]();
    }
    for (i = 0; i < 8; i++) {
        printf(" %d", lr[i]);
    }
    printf("\n");
    DUMP(lsr);

    printf("%d %d %d %d\n", guv.a, guv.b, guv2.a, guv2.b);
    printf("%d %d %d %d\n", ga.k, ga.lo, ga.hi, ga.z);
    printf("%d %d %d\n", ga2.k, ga2.i, ga2.z);
    printf("%d %d %d %d %d\n", luv.a, luv.b, la.lo, la.hi, la.z);
    return 0;
}
