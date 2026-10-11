/* variadic calls and va_list in both directions */
#include <stdarg.h>
long vsum(int n, ...);
double vmix(const char *fmt, ...);
long vpass(int n, va_list ap);
long caller_vpass(int n, va_list ap);
long callee_calls_caller(void);
