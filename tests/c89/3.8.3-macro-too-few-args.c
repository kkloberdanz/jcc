/* C89 3.8.3 macro replacement: the arguments of a function-like macro invocation shall match its parameters */
#define F(a, b) (a + b)
int main(void) { return F(1); /* violation */ }
