/* C89 3.5.2.1 structure specifiers: a structure shall not contain a member with function type */
struct s { int f(void); }; /* violation */
int main(void) { return 0; }
