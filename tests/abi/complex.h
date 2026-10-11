/* complex values across the compiler boundary: arguments, returns, records, variadic */
struct CRec { int tag; float _Complex f; double _Complex d; };
double _Complex c_add(double _Complex a, double _Complex b, double k);
float _Complex c_mulf(float _Complex a, float _Complex b);
long double _Complex c_divl(long double _Complex a, long double _Complex b);
struct CRec c_rec(int tag, float _Complex f, double _Complex d);
double c_sum_rec(struct CRec r);
double _Complex c_vsum(int n, ...);
double _Complex c_many(double d0, double d1, double d2, double d3, double d4, double d5, double d6,
                       double _Complex a, float _Complex b, int i, long double _Complex c);
double _Complex caller_twice(double _Complex z);
