/* C89 3.6.4.2 the switch statement: no two case expressions shall have the same value */
int main(void) { switch (1) { case 1: return 1; case 2 - 1: /* violation */ return 2; } return 0; }
