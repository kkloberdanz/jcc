/* C89 3.5.4.2 array declarators: the element type shall not be incomplete */
struct s;
struct s a[3]; /* violation */
int main(void) { return 0; }
