/* C89 3.3.2.2 function calls: each argument shall be assignable to its parameter's unqualified type */
struct s { int a; };
int f(int x) { return x; }
int main(void) { struct s v = {1}; return f(v); /* violation */ }
