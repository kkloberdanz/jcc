/* C89 3.3.16 assignment operators: a const-qualified object is not a modifiable lvalue */
int main(void) { const int c = 1; c = 2; /* violation */ return c; }
