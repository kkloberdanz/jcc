#include <stdio.h>
#include <string.h>
#include "pack.h"
int main(void) {
    struct P1 p1; struct P2 p2; struct N n; struct P1b p1b; struct P4 p4;
    struct P1c p1c; struct Nb nb; struct Nested ne; struct Bits bits;
    unsigned char raw[64];
    int i;
    print_sizes("caller");
    print_sizes("callee");
    fill_all(&p1, &p2, &n, &p1b, &p4, &p1c, &nb, &ne, &bits);
    print_all(&p1, &p2, &n, &p1b, &p4, &p1c, &nb, &ne, &bits);
    printf("caller: %d %d %d %ld | %d %d %d %ld | %d %d %d %ld | %d %g | %d %g %ld | %d %d | %d %d | %d %d %d %d %ld %d | %u %u %d %u\n",
           p1.a, p1.b, p1.c, p1.d, p2.a, p2.b, p2.c, p2.d, n.a, n.b, n.c, n.d,
           p1b.a, p1b.b, p4.a, p4.b, p4.c, p1c.a, p1c.b, nb.a, nb.b,
           ne.a, ne.p.a, ne.p.b, ne.p.c, ne.p.d, ne.b, bits.a, bits.b, bits.c, bits.d);
    /* the bytes themselves, so padding and bit placement are compared too */
    memcpy(raw, &p1, sizeof p1);
    for (i = 0; i < (int)sizeof p1; i++) printf("%02x", raw[i]);
    memcpy(raw, &bits, sizeof bits);
    printf(" ");
    for (i = 0; i < (int)sizeof bits; i++) printf("%02x", raw[i]);
    printf("\n");
    p1 = by_value(p1, p1c);
    printf("caller: by_value %d %d %d %ld\n", p1.a, p1.b, p1.c, p1.d);
    return (int)sizeof(struct P1) + (int)sizeof(struct P4);
}
