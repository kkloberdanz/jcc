/* C89 3.3.6 additive operators: a pointer may only be added to an integral type */
int main(void) { int x, *p = &x; return p + 1.0 == p; /* violation */ }
