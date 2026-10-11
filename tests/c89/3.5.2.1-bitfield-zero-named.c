/* C89 3.5.2.1 structure specifiers: a zero-width bit-field shall have no declarator */
struct s { int b : 0; }; /* violation */
int main(void) { return 0; }
