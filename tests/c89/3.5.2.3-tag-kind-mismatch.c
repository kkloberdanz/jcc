/* C89 3.5.2.3 tags: a tag names one kind of type throughout its scope */
struct s { int a; };
union s u; /* violation */
int main(void) { return 0; }
