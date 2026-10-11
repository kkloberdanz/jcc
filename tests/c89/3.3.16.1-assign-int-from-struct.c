/* C89 3.3.16.1 simple assignment: an arithmetic left operand needs an arithmetic right operand */
struct s { int a; };
int main(void) { struct s v = {1}; int i; i = v; /* violation */ return i; }
