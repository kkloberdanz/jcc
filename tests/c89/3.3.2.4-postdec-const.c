/* C89 3.3.2.4 postfix decrement: the operand shall be a modifiable lvalue */
int main(void) { const int c = 1; c--; /* violation */ return 0; }
