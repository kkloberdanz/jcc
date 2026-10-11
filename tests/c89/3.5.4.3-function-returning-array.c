/* C89 3.5.4.3 function declarators: a function shall not return an array type */
int f(void)[3]; /* violation */
int main(void) { return 0; }
