/* C89 3.6.1 labeled statements: a default label shall appear only in a switch */
int main(void) { default: /* violation */ return 0; }
