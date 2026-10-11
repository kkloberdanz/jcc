/* C89 3.5.2.1 structure specifiers: a structure shall not contain a member with incomplete type */
struct s { void v; }; /* violation */
int main(void) { return 0; }
