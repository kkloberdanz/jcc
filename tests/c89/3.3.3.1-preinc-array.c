/* C89 3.3.3.1 prefix increment: the operand shall be a modifiable lvalue (an array is not) */
int main(void) { int a[2]; ++a; /* violation */ return 0; }
