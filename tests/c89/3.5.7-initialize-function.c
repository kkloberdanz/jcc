/* C89 3.5.7 initialization: the entity initialized shall have object type */
int f(void) = 0; /* violation */
int main(void) { return 0; }
