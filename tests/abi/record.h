/* records passed and returned by value across the compiler boundary */
struct S8 { int a; int b; };
struct S12 { int a; short b; char c; int d; };
struct S16 { long a; long b; };
struct S24 { long a; long b; long c; };
struct Sc { char a, b, c; };
union U { long l; char c[8]; };

struct S8 make8(int a, int b);
struct S16 make16(long a, long b);
struct S24 make24(long a, long b, long c);
struct Sc makec(char a, char b, char c);
union U makeu(long l);
long sum8(struct S8 s);
long sum12(struct S12 s);
long sum16(struct S16 s);
long sum24(struct S24 s);
long sumc(struct Sc s);
long sumu(union U u);
long mixed(int x, struct S24 a, struct S8 b, long y, struct S16 c, char z);
struct S24 pass_through(struct S24 s, int bump);
/* the caller's side, called back from the callee */
long caller_sum(struct S8 a, struct S16 b, struct S24 c);
struct S12 caller_make12(int a, short b, char c, int d);
