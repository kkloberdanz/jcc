/* C89 3.3.2.4 postfix increment: the operand shall have scalar type */
struct s { int a; };
int main(void) { struct s v = {1}; v++; /* violation */ return 0; }
