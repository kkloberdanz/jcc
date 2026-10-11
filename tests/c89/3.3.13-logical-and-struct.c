/* C89 3.3.13 logical AND: each operand shall have scalar type */
struct s { int a; };
int main(void) { struct s v = {1}; return v && 1; /* violation */ }
