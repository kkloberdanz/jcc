/* C89 3.3.16.2 compound assignment: only += and -= accept a pointer left operand */
int main(void) { int x, *p = &x; p *= 2; /* violation */ return 0; }
