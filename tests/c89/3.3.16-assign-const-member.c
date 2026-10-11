/* C89 3.3.16 assignment operators: a structure with a const member is not a modifiable lvalue */
struct s { const int a; };
int main(void) { struct s v = {1}, w = {2}; v = w; /* violation */ return v.a; }
