/* C89 3.3.4 cast operators: the type name shall specify a scalar type or void */
struct s { int a; };
int main(void) { struct s v = (struct s)1; /* violation */ return v.a; }
