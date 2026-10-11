/* The preprocessor: macros, conditionals, line control and includes. */
#include <stdio.h>
#include <string.h>
#include "include/cpptest.h"
#include "include/cpptest.h"

#define STR(x) #x
#define XSTR(x) STR(x)
#define CAT(a, b) a##b
#define XCAT(a, b) CAT(a, b)
#define EMPTY
#define ID(x) x
#define TWICE(x) ((x) + (x))
#define ARGS(...) f(__VA_ARGS__)
#define LOG(fmt, ...) snprintf(buf, sizeof buf, fmt, ##__VA_ARGS__)
#define GNU(fmt, args...) snprintf(buf, sizeof buf, fmt, args)
#define APPLY(m, x) m(x)
#define f(x) (x + 1)
#define VALUE 40
#define SELF SELF + 1
#define INDIRECT ID(SELF)
#define PASTE_EMPTY(a, b) a##b
#define COMMA ,
#define LIST(a) a COMMA a

static int f_real(int a, int b, int c) { return a + b + c; }
#undef f
#define f(a, b, c) f_real(a, b, c)

int main(void)
{
    char buf[64];
    int total = 0;
    int XCAT(var, 1) = 3;           /* var1 */
    int CAT(var, 2) = 4;            /* var2 */
    const char *s = STR(a  "b"  'c' \n  d);
    const char *v = XSTR(VALUE);
    const char *w = STR(VALUE);
    int arr[] = { LIST(5) };
    int line_before = __LINE__;

    total += var1 + var2;                       /* 7 */
    total += (int)strlen(s);                    /* a "b" 'c' \n d */
    total += (int)strlen(v) + (int)strlen(w);   /* 2 + 5 */
    total += TWICE(var1);                       /* 6 */
    total += ARGS(1, 2, 3);                     /* 7 */
    total += APPLY(TWICE, 10);                  /* 20 */
    total += PASTE_EMPTY(1, ) + PASTE_EMPTY(, 2);       /* 1 + 2 */
    total += arr[0] + arr[1];                   /* 10 */
    total += FROM_HEADER + header_fn();         /* 7 + 8 */
    total += HEADER_LINE;                       /* 6 */
    total += __LINE__ - line_before;            /* 17 */
#line 100
    total += __LINE__;                          /* 100 */
#line 200 "renamed.c"
    total += (int)strlen(__FILE__);             /* 9 */
    total += __LINE__;                          /* 201 */

    LOG("%d", 5);
    total += (int)strlen(buf);                  /* 1 */
    LOG("x");
    total += (int)strlen(buf);                  /* 1 */
    GNU("%d%d", 1, 2);
    total += (int)strlen(buf);                  /* 2 */

#if defined(VALUE) && VALUE == 40 && !defined(UNDEFINED) && (1 ? 2 : 1 / 0) == 2
    total += 1000;
#elif 1
#error not reached
#else
#error not reached either
#endif
#if 'A' == 65 && '\n' == 10 && 0x10 == 16 && 010 == 8 && -1 < 0 && -1 > 0u && (2 || 1 / 0) && !(0 && 1 / 0)
    total += 2000;
#endif
#if (1 << 3) - 1 == 7 && 7 % 3 == 1 && ~0 == -1 && (0xffffffffffffffff > 0) && UNDEFINED_NAME == 0
    total += 4000;
#endif
#ifdef __STDC__
    total += __STDC__;                          /* 1 */
#endif
#if __INCLUDE_LEVEL__ == 0 && __COUNTER__ == 0 && __COUNTER__ == 1
    total += 8000;
#endif
    puts(XSTR(total is CAT(com, puted)));       /* "total is computed" */
    printf("%d\n", total);
    return total % 256;
}
