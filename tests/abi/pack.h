/* #pragma pack: layout must agree on both sides of the boundary */
#pragma pack(1)
struct P1 { char a; int b; short c; long d; };
#pragma pack(2)
struct P2 { char a; int b; short c; long d; };
#pragma pack()
struct N { char a; int b; short c; long d; };
#pragma pack(push, 1)
struct P1b { char a; double b; };
#pragma pack(push, 4)
struct P4 { char a; double b; long c; };
#pragma pack(pop)
struct P1c { short a; int b; };
#pragma pack(pop)
struct Nb { short a; int b; };
struct Nested { char a; struct P1 p; char b; };
#pragma pack(1)
struct Bits { unsigned a : 3; unsigned b : 13; char c; unsigned d : 20; };
#pragma pack()

void fill_all(struct P1 *p1, struct P2 *p2, struct N *n, struct P1b *p1b, struct P4 *p4,
              struct P1c *p1c, struct Nb *nb, struct Nested *ne, struct Bits *bits);
void print_all(const struct P1 *p1, const struct P2 *p2, const struct N *n, const struct P1b *p1b,
               const struct P4 *p4, const struct P1c *p1c, const struct Nb *nb,
               const struct Nested *ne, const struct Bits *bits);
void print_sizes(const char *who);
struct P1 by_value(struct P1 p, struct P1c q);
