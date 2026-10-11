/* C89 3.6.4.2 the switch statement: the controlling expression shall have integral type */
int main(void) { switch (1.0) { /* violation */ case 1: return 1; } return 0; }
