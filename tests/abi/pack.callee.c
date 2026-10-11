#include <stdio.h>
#include <stddef.h>
#include "pack.h"
void fill_all(struct P1 *p1, struct P2 *p2, struct N *n, struct P1b *p1b, struct P4 *p4,
              struct P1c *p1c, struct Nb *nb, struct Nested *ne, struct Bits *bits) {
    p1->a = 1; p1->b = 2; p1->c = 3; p1->d = 4;
    p2->a = 5; p2->b = 6; p2->c = 7; p2->d = 8;
    n->a = 9; n->b = 10; n->c = 11; n->d = 12;
    p1b->a = 13; p1b->b = 14.5;
    p4->a = 15; p4->b = 16.5; p4->c = 17;
    p1c->a = 18; p1c->b = 19;
    nb->a = 20; nb->b = 21;
    ne->a = 22; ne->p = *p1; ne->b = 23;
    bits->a = 5; bits->b = 4321; bits->c = 24; bits->d = 654321;
}
void print_all(const struct P1 *p1, const struct P2 *p2, const struct N *n, const struct P1b *p1b,
               const struct P4 *p4, const struct P1c *p1c, const struct Nb *nb,
               const struct Nested *ne, const struct Bits *bits) {
    printf("callee: %d %d %d %ld | %d %d %d %ld | %d %d %d %ld | %d %g | %d %g %ld | %d %d | %d %d | %d %d %d %d %ld %d | %u %u %d %u\n",
           p1->a, p1->b, p1->c, p1->d, p2->a, p2->b, p2->c, p2->d, n->a, n->b, n->c, n->d,
           p1b->a, p1b->b, p4->a, p4->b, p4->c, p1c->a, p1c->b, nb->a, nb->b,
           ne->a, ne->p.a, ne->p.b, ne->p.c, ne->p.d, ne->b, bits->a, bits->b, bits->c, bits->d);
}
void print_sizes(const char *who) {
    printf("%s: P1 %d %d %d %d %d\n", who, (int)sizeof(struct P1), (int)offsetof(struct P1, a),
           (int)offsetof(struct P1, b), (int)offsetof(struct P1, c), (int)offsetof(struct P1, d));
    printf("%s: P2 %d %d %d %d %d\n", who, (int)sizeof(struct P2), (int)offsetof(struct P2, a),
           (int)offsetof(struct P2, b), (int)offsetof(struct P2, c), (int)offsetof(struct P2, d));
    printf("%s: N %d %d %d %d %d\n", who, (int)sizeof(struct N), (int)offsetof(struct N, a),
           (int)offsetof(struct N, b), (int)offsetof(struct N, c), (int)offsetof(struct N, d));
    printf("%s: P1b %d %d\n", who, (int)sizeof(struct P1b), (int)offsetof(struct P1b, b));
    printf("%s: P4 %d %d %d\n", who, (int)sizeof(struct P4), (int)offsetof(struct P4, b), (int)offsetof(struct P4, c));
    printf("%s: P1c %d %d\n", who, (int)sizeof(struct P1c), (int)offsetof(struct P1c, b));
    printf("%s: Nb %d %d\n", who, (int)sizeof(struct Nb), (int)offsetof(struct Nb, b));
    printf("%s: Nested %d %d %d\n", who, (int)sizeof(struct Nested), (int)offsetof(struct Nested, p), (int)offsetof(struct Nested, b));
    printf("%s: Bits %d %d\n", who, (int)sizeof(struct Bits), (int)offsetof(struct Bits, c));
}
struct P1 by_value(struct P1 p, struct P1c q) {
    printf("callee: by_value %d %d %d %ld %d %d\n", p.a, p.b, p.c, p.d, q.a, q.b);
    p.b += q.b;
    return p;
}
