/* C89 3.5.7 initialization: the entity initialized shall have object type */
struct s;
struct s v = {1}; /* violation */
int main(void) { return 0; }
