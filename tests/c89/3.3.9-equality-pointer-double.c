/* C89 3.3.9 equality operators: a pointer can only be compared with a pointer or a null pointer constant */
int main(void) { int x, *p = &x; return p == 1.0; /* violation */ }
