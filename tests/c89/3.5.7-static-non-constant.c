/* C89 3.5.7 initialization: a static object's initializer shall be constant */
int n = 1;
int main(void) { static int x = n; /* violation */ return x; }
