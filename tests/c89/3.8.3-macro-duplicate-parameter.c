/* C89 3.8.3 macro replacement: macro parameter names shall be unique */
#define F(a, a) (a) /* violation */
int main(void) { return 0; }
