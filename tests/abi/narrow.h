/* more arguments than registers, with narrow types among the overflow */
long many(char a, short b, int c, long d, unsigned char e, unsigned short f,
          int g, long h, char i, short j, unsigned char k, unsigned short l,
          int m, char n);
double manyf(double a, double b, double c, double d, double e, double f, double g, double h,
             float i, double j, float k, char l, short m);
long callback(char a, short b, int c, long d, unsigned char e, unsigned short f,
              int g, long h, char i, short j, unsigned char k, unsigned short l,
              int m, char n);
long callee_calls_back(void);
