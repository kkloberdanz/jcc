/* C89 3.3.2.1 array subscripting: the index shall have integral type */
int main(void) { int a[2]; return a[1.5]; /* violation */ }
