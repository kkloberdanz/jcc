/* C89 3.5 declarations: an identifier with no linkage shall be declared only once in a scope */
int main(void) { int x = 1; int x = 2; /* violation */ return x; }
