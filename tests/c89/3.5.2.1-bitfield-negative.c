/* C89 3.5.2.1 structure specifiers: a bit-field width shall be nonnegative */
struct s { int b : -1; }; /* violation */
int main(void) { return 0; }
