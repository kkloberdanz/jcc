/* C89 3.5.2.2 enumeration specifiers: an enumerator's value shall be an integral constant expression */
enum e { A = 1.5 }; /* violation */
int main(void) { return A; }
