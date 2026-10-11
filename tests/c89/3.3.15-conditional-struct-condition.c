/* C89 3.3.15 conditional operator: the first operand shall have scalar type */
struct s { int a; };
int main(void) { struct s v = {1}; return v ? 1 : 2; /* violation */ }
