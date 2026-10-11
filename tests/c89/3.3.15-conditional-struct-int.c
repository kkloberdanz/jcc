/* C89 3.3.15 conditional operator: the second and third operands shall have compatible types */
struct s { int a; };
int main(void) { struct s v = {1}; return (1 ? v : 2).a; /* violation */ }
