/* C89 3.3.7 bitwise shift operators: each operand shall have integral type */
int main(void) { return 1.0 << 1; /* violation */ }
