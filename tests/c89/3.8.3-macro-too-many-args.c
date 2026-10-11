/* C89 3.8.3 macro replacement: the arguments of a function-like macro invocation shall match its parameters */
#define F(a) (a)
int main(void) { return F(1, 2); /* violation */ }
