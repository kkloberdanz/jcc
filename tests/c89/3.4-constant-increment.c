/* C89 3.4 constant expressions: shall not contain an increment */
int x;
enum { A = x++ }; /* violation */
int main(void) { return A; }
