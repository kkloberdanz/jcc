/* C89 3.3.6 additive operators: pointers subtracted shall point to compatible object types */
int main(void) { int x; double d; return &x - &d; /* violation */ }
