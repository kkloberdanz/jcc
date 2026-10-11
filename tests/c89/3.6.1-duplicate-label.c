/* C89 3.6.1 labeled statements: label names shall be unique within a function */
int main(void) { l: ; l: /* violation */ return 0; }
