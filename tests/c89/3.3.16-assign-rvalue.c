/* C89 3.3.16 assignment operators: the left operand shall be an lvalue */
int main(void) { int x = 1; x + 1 = 2; /* violation */ return x; }
