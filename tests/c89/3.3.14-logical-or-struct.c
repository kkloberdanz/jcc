/* C89 3.3.14 logical OR: each operand shall have scalar type */
struct s { int a; };
int main(void) { struct s v = {1}; return 1 || v; /* violation */ }
