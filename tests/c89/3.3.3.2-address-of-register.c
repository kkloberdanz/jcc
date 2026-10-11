/* C89 3.3.3.2 address operator: the operand shall not be declared register */
int main(void) { register int r = 1; int *p = &r; /* violation */ return *p; }
