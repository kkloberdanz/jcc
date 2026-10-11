/* C89 3.3.3.2 address operator: the operand shall not be a bit-field */
struct s { int b : 3; };
int main(void) { struct s v = {1}; int *p = &v.b; /* violation */ return *p; }
