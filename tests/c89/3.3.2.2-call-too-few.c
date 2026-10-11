/* C89 3.3.2.2 function calls: with a prototype, the number of arguments shall agree with the number of parameters */
int f(int a, int b) { return a + b; }
int main(void) { return f(1); /* violation */ }
