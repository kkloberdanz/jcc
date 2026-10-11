/* C89 3.5.2.2 enumeration specifiers: an enumerator's value shall be an integral constant expression */
int n = 1;
enum e { A = n }; /* violation */
int main(void) { return A; }
