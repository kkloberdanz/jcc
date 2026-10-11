/* C89 3.7.1 function definitions: the storage class shall be extern or static */
auto int f(void) { return 0; } /* violation */
int main(void) { return f(); }
