/* C89 3.3.3.2 address operator: the operand shall be a function designator or an lvalue */
int main(void) { int x = 1; int *p = &(x + 1); /* violation */ return *p; }
