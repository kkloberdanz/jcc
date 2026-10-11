/* C89 3.6.4.2 the switch statement: at most one default label */
int main(void) { switch (1) { default: return 1; default: /* violation */ return 2; } return 0; }
