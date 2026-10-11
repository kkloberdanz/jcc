/* C89 3.4 constant expressions: shall not contain a function call */
int f(void) { return 1; }
enum { A = f() }; /* violation */
int main(void) { return A; }
