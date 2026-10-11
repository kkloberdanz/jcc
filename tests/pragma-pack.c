/* #pragma pack: sizes, offsets, bit placement and access, compared with cc */
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#pragma pack(1)
struct B1 { unsigned a : 3; unsigned b : 13; char c; unsigned d : 20; };
struct B1b { char c; unsigned d : 20; short e; unsigned f : 30; };
struct B1c { char a; int b; };
#pragma pack(2)
struct B2 { unsigned a : 3; unsigned b : 13; char c; unsigned d : 20; };
struct B2b { char c; unsigned d : 20; short e; unsigned f : 30; long g; };
struct B2c { char a; double b; };
#pragma pack(4)
struct B4 { char a; double b; long c; unsigned d : 20; char e; unsigned f : 30; };
#pragma pack()
struct B0 { unsigned a : 3; unsigned b : 13; char c; unsigned d : 20; };
#pragma pack(push, 1)
struct A2 { char a; int b; } __attribute__((aligned(8)));
union U1 { char a; int b; };
#pragma pack(push, 8)
struct N8 { char a; int b; };
#pragma pack(pop)
struct P1 { char a; short b; };
#pragma pack(pop)
struct N0 { char a; short b; };
_Pragma("pack(2)")
struct Q2 { char a; long b; };
_Pragma("pack()")
struct Nested { char a; struct B1 b; char c; struct Q2 q; };
/* a static initializer of packed bit-fields, merged byte by byte */
static struct B1 sb1 = { 5, 4321, 24, 654321 };
static struct B4 sb4 = { 1, 2.5, 3, 777777, 4, 1000000000 };

#define SZ(T) printf("%s size %d align %d\n", #T, (int)sizeof(T), (int)offsetof(struct { char c; T t; }, t))
#define OFF(S, M) printf("%s.%s off %d\n", #S, #M, (int)offsetof(struct S, M))
static void dump(const char *what, const void *p, size_t n) {
    const unsigned char *b = p;
    size_t i;
    printf("%s:", what);
    for (i = 0; i < n; i++) printf(" %02x", b[i]);
    printf("\n");
}
int main(void) {
    struct B1 b1; struct B1b b1b; struct B2 b2; struct B2b b2b; struct B4 b4; struct Nested ne;
    SZ(struct B1); SZ(struct B1b); SZ(struct B1c); SZ(struct B2); SZ(struct B2b); SZ(struct B2c);
    SZ(struct B4); SZ(struct B0); SZ(struct A2); SZ(union U1); SZ(struct N8); SZ(struct P1); SZ(struct N0);
    SZ(struct Q2); SZ(struct Nested);
    OFF(B1, c); OFF(B1b, c); OFF(B1b, e); OFF(B1c, b); OFF(B2, c); OFF(B2b, e); OFF(B2b, g); OFF(B2c, b);
    OFF(B4, b); OFF(B4, c); OFF(B4, e); OFF(B0, c); OFF(A2, b); OFF(N8, b); OFF(P1, b); OFF(N0, b); OFF(Q2, b);
    OFF(Nested, b); OFF(Nested, c); OFF(Nested, q);
    memset(&b1, 0, sizeof b1); b1.a = 5; b1.b = 4321; b1.c = 24; b1.d = 654321;
    dump("b1", &b1, sizeof b1);
    printf("%u %u %d %u\n", b1.a, b1.b, b1.c, b1.d);
    memset(&b1b, 0xff, sizeof b1b); b1b.c = 1; b1b.d = 0x12345; b1b.e = -2; b1b.f = 0x3abcdef0;
    dump("b1b", &b1b, sizeof b1b);
    printf("%d %u %d %u\n", b1b.c, b1b.d, b1b.e, b1b.f);
    memset(&b2, 0, sizeof b2); b2.a = 7; b2.b = 0x1fff; b2.c = -1; b2.d = 0xfffff;
    dump("b2", &b2, sizeof b2);
    printf("%u %u %d %u\n", b2.a, b2.b, b2.c, b2.d);
    memset(&b2b, 0, sizeof b2b); b2b.c = 9; b2b.d = 0xabcde; b2b.e = 0x7fff; b2b.f = 0x2fffffff; b2b.g = -1L;
    dump("b2b", &b2b, sizeof b2b);
    printf("%d %u %d %u %ld\n", b2b.c, b2b.d, b2b.e, b2b.f, b2b.g);
    memset(&b4, 0, sizeof b4); b4.a = 1; b4.b = 2.5; b4.c = 3; b4.d = 777777; b4.e = 4; b4.f = 1000000000;
    dump("b4", &b4, sizeof b4);
    printf("%d %g %ld %u %d %u\n", b4.a, b4.b, b4.c, b4.d, b4.e, b4.f);
    b4.d += 1; b4.f -= 7; b4.d++;
    printf("%u %u\n", b4.d, b4.f);
    dump("sb1", &sb1, sizeof sb1);
    dump("sb4", &sb4, sizeof sb4);
    printf("%u %u %d %u | %d %g %ld %u %d %u\n", sb1.a, sb1.b, sb1.c, sb1.d, sb4.a, sb4.b, sb4.c, sb4.d, sb4.e, sb4.f);
    memset(&ne, 0, sizeof ne); ne.a = 1; ne.b = b1; ne.c = 2; ne.q.a = 3; ne.q.b = 0x1122334455667788L;
    dump("ne", &ne, sizeof ne);
    {
        #pragma pack(1)
        struct L { char a; int b; } l;
        #pragma pack()
        struct M { char a; int b; } m;
        printf("in block: %d %d\n", (int)sizeof l, (int)sizeof m);
    }
    return (int)sizeof(struct B2b);
}
