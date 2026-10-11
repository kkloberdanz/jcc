/* C89 3.3.4 cast operators: the operand shall have scalar type */
struct s { int a; };
int main(void) { struct s v = {1}; return (int)v; /* violation */ }
