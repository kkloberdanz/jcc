/* C89 3.8.8 predefined macro names: defined shall not be the subject of #define */
#define defined 1 /* violation */
int main(void) { return 0; }
