/* C89 3.5.4.3 function declarators: parameters of a definition shall not have incomplete type */
struct s;
int f(struct s x) { return 0; } /* violation */
int main(void) { return 0; }
