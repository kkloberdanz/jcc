/* C89 3.5.2 type specifiers: signed and unsigned shall not be combined */
unsigned signed x; /* violation */
int main(void) { return 0; }
