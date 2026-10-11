/* C89 3.5.2.1 structure specifiers: a bit-field width shall not exceed the width of its type */
struct s { int b : 33; }; /* violation */
int main(void) { return 0; }
