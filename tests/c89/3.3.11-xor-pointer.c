/* C89 3.3.11 bitwise exclusive OR: each operand shall have integral type */
int main(void) { int x, *p = &x; return p ^ 1; /* violation */ }
