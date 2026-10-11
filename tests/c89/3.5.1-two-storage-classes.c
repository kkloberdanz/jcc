/* C89 3.5.1 storage-class specifiers: at most one may appear in a declaration */
static extern int x; /* violation */
int main(void) { return 0; }
