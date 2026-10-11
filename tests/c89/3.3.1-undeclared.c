/* C89 3.3.1 primary expressions: an identifier is a primary expression only if it has been declared */
int main(void) { return undeclared_name; /* violation */ }
