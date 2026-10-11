/* C89 3.5.4.2 array declarators: the size shall be an integral constant expression */
int n = 3;
int a[n]; /* violation */
int main(void) { return 0; }
