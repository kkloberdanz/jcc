/* C89 3.5.4.2 array declarators: the size shall be an integral constant expression */
int a[2.0]; /* violation */
int main(void) { return 0; }
