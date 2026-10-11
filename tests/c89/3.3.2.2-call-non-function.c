/* C89 3.3.2.2 function calls: the called expression shall have type pointer to function */
int main(void) { int x = 1; return x(); /* violation */ }
