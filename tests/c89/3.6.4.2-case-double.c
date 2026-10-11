/* C89 3.6.4.2 the switch statement: a case expression shall be an integral constant expression */
int main(void) { switch (1) { case 1.0: /* violation */ return 1; } return 0; }
