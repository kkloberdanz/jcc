/* C89 3.3.3.4 sizeof: shall not be applied to a bit-field member */
struct s { int b : 3; };
int main(void) { struct s v = {1}; return sizeof v.b; /* violation */ }
