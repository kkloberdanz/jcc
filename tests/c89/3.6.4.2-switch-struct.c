/* C89 3.6.4.2 the switch statement: the controlling expression shall have integral type */
struct s { int a; };
int main(void) { struct s v = {1}; switch (v) { /* violation */ case 1: return 1; } return 0; }
