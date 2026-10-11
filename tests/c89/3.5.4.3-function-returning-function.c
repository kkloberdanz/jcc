/* C89 3.5.4.3 function declarators: a function shall not return a function type */
int f(void)(void); /* violation */
int main(void) { return 0; }
