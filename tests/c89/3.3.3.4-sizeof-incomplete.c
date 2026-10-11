/* C89 3.3.3.4 sizeof: shall not be applied to an incomplete type */
struct s;
int main(void) { return sizeof(struct s); /* violation */ }
