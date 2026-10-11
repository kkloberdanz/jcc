/* C89 3.8.1 conditional inclusion: #elif shall not follow #else */
#if 1
#else
#elif 1 /* violation */
#endif
int main(void) { return 0; }
