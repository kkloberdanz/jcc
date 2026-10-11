/* C89 3.5.2.3 tags: a type's content shall be defined at most once */
struct s { int a; };
struct s { int a; }; /* violation */
int main(void) { return 0; }
