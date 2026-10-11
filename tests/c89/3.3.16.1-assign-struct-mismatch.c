/* C89 3.3.16.1 simple assignment: structure operands shall have compatible types */
struct s { int a; }; struct u { int a; };
int main(void) { struct s v = {1}; struct u w = {2}; v = w; /* violation */ return v.a; }
