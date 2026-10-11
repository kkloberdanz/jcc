/* C89 3.3.5 multiplicative operators: each operand shall have arithmetic type */
int main(void) { int x, *p = &x; return p * 2; /* violation */ }
