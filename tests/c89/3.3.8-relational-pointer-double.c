/* C89 3.3.8 relational operators: a pointer can only be compared with a pointer */
int main(void) { int x, *p = &x; return p < 1.0; /* violation */ }
