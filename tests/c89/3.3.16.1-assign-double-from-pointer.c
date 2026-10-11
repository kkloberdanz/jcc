/* C89 3.3.16.1 simple assignment: an arithmetic left operand needs an arithmetic right operand */
int main(void) { int x; double d; d = &x; /* violation */ return (int)d; }
