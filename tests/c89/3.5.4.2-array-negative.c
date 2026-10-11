/* C89 3.5.4.2 array declarators: the size shall be greater than zero */
int a[-1]; /* violation */
int main(void) { return 0; }
