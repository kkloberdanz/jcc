/* C89 3.8.3.3 the ## operator: shall not appear at either end of a replacement list */
#define F(a) ## a /* violation */
int main(void) { return 0; }
