/* C89 3.4 constant expressions: shall not contain an assignment */
int x;
enum { A = (x = 1) }; /* violation */
int main(void) { return A; }
