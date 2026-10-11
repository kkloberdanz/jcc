/* C89 3.6.4.2 the switch statement: a case expression shall be an integral constant expression */
int main(void) { int n = 1; switch (n) { case n: /* violation */ return 1; } return 0; }
