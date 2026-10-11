/* C89 3.5.2.1 structure specifiers: a bit-field shall have type int, signed int or unsigned int */
struct s { double b : 3; }; /* violation */
int main(void) { return 0; }
