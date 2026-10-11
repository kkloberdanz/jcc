/* C89 3.6.6.1 the goto statement: the label shall be in the current function */
int main(void) { goto nowhere; /* violation */ return 0; }
