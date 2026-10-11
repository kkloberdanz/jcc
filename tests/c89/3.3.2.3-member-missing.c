/* C89 3.3.2.3 structure and union members: the identifier shall name a member of the type */
struct s { int a; };
int main(void) { struct s v = {1}; return v.b; /* violation */ }
