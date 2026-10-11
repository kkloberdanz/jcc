/* C89 3.5.2.1 structure specifiers: a bit-field width shall be an integral constant expression */
int n = 3;
struct s { int b : n; }; /* violation */
int main(void) { return 0; }
