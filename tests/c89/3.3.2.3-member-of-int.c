/* C89 3.3.2.3 structure and union members: the operand of . shall have structure or union type */
int main(void) { int x = 0; return x.m; /* violation */ }
