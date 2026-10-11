/* C89 3.6.1 labeled statements: a case label shall appear only in a switch */
int main(void) { case 1: /* violation */ return 0; }
