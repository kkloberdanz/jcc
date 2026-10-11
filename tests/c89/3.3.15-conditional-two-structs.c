/* C89 3.3.15 conditional operator: structure operands shall have the same type */
struct s { int a; }; struct u { int a; };
int main(void) { struct s v = {1}; struct u w = {2}; return (1 ? v : w).a; /* violation */ }
