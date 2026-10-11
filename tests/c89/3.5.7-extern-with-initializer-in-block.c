/* C89 3.5.7 initialization: a block-scope declaration with linkage shall have no initializer */
int x;
int main(void) { extern int x = 1; /* violation */ return x; }
