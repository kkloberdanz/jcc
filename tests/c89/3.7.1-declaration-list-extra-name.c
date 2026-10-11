/* C89 3.7.1 function definitions: the declaration list shall declare only identifiers from the identifier list */
int f(a) int a; int c; { return a; } /* violation */
int main(void) { return f(1); }
