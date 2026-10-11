/* C89 3.3.2.3 structure and union members: the operand of -> shall be a pointer to a structure or union */
struct s { int a; };
int main(void) { struct s v = {1}; return v->a; /* violation */ }
