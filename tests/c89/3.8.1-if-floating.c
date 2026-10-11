/* C89 3.8.1 conditional inclusion: the expression shall be an integral constant expression */
#if 1.0 /* violation */
#endif
int main(void) { return 0; }
