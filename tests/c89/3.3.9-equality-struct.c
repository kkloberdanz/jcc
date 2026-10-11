/* C89 3.3.9 equality operators: operands shall both be arithmetic or both pointers */
struct s { int a; };
int main(void) { struct s v = {1}; return v == v; /* violation */ }
