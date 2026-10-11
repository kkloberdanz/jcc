/* C89 3.3.3.3 logical negation: the operand shall have scalar type */
struct s { int a; };
int main(void) { struct s v = {1}; return !v; /* violation */ }
