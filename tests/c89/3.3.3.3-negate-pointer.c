/* C89 3.3.3.3 unary minus: the operand shall have arithmetic type */
int main(void) { int x, *p = &x; return -p; /* violation */ }
