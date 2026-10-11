/* C89 3.5.2.1 structure specifiers: a structure shall not contain a member with incomplete type */
struct s { struct s next; }; /* violation */
int main(void) { return 0; }
