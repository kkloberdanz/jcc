/* C89 3.3.6 additive operators: for + either both operands are arithmetic or one is a pointer and the other integral */
int main(void) { int x, *p = &x, *q = &x; return p + q == p; /* violation */ }
