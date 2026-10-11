/* C89 3.3.6 additive operators: an integral left operand cannot have a pointer subtracted from it */
int main(void) { int x, *p = &x; return 1 - p; /* violation */ }
