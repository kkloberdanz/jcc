/* C89 3.5.4.2 array declarators: the element type shall not be a function type */
int a[3](void); /* violation */
int main(void) { return 0; }
