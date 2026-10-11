/* long double across the compiler boundary: in both directions, in memory, in records, variadic */
struct LDRec { int tag; long double v; double w; };
long double ld_add(long double a, int b, long double c, double d);
struct LDRec ld_rec(long double v, int tag);
long double ld_sum_rec(struct LDRec r, long double extra);
long double ld_vsum(int n, ...);
long double ld_many(double d0, double d1, double d2, double d3, double d4, double d5,
                    double d6, double d7, double d8, long double l0, int i0, long double l1);
long double caller_check(long double seed);
