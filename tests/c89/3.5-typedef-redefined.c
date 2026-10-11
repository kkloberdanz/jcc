/* C89 3.5 declarations: a typedef name has no linkage and shall be declared only once in a scope */
typedef int T;
typedef double T; /* violation */
int main(void) { T x = 0; return x; }
