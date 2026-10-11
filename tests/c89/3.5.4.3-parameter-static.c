/* C89 3.5.4.3 function declarators: register is the only storage class allowed on a parameter */
int f(static int x); /* violation */
int main(void) { return 0; }
