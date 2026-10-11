/* C89 3.3.3.2 indirection operator: the operand shall have pointer type */
int main(void) { int x = 1; return *x; /* violation */ }
