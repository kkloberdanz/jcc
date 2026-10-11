/* C89 3.3.16.1 simple assignment: a pointer left operand needs a pointer or null pointer constant */
int main(void) { int *p; p = 1.0; /* violation */ return p == 0; }
