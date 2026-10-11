/* C89 3.7 external definitions: an identifier with internal linkage shall have at most one external definition */
static int x = 1;
static int x = 2; /* violation */
int main(void) { return x; }
