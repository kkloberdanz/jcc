/* C89 3.6.6.4 the return statement: a void function shall not return a value */
void f(void) { return 1; } /* violation */
int main(void) { f(); return 0; }
