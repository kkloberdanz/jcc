/* C89 3.3.12 bitwise inclusive OR: each operand shall have integral type */
int main(void) { return 1 | 2.0; /* violation */ }
