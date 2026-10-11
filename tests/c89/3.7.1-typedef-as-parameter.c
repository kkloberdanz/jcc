/* C89 3.7.1 function definitions: a typedef name shall not be redeclared as a parameter */
typedef int T;
int f(T) int T; { return T; } /* violation */
int main(void) { return f(1); }
