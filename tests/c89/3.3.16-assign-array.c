/* C89 3.3.16 assignment operators: the left operand shall be a modifiable lvalue (an array is not) */
int main(void) { int a[2], b[2] = {1, 2}; a = b; /* violation */ return a[0]; }
