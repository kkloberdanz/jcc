/* C89 3.3.16.2 compound assignment: the operands shall have arithmetic type */
struct s { int a; };
int main(void) { struct s v = {1}; v += 1; /* violation */ return 0; }
