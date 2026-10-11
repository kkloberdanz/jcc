/* C89 3.3.2.2 function calls: each argument shall be assignable to its parameter's unqualified type */
int f(int *p) { return *p; }
int main(void) { return f(1.0); /* violation */ }
