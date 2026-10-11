/* Variadic functions: a record passed by reference before the variable
   arguments, and side effects in the argument of va_end. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { char a[32]; } big;

static void f(big x, char *s, ...)
{
    va_list ap;
    int a, b, c;
    va_start(ap, s);
    a = va_arg(ap, int);
    b = va_arg(ap, int);
    c = va_arg(ap, int);
    printf("%c%s %d %d %d\n", x.a[0], s, a, b, c);
    va_end(ap);
}

static void g(big x, big y, double d, ...)
{
    va_list ap;
    int n;
    char *s;
    va_start(ap, d);
    n = va_arg(ap, int);
    s = va_arg(ap, char *);
    printf("%c %c %g %d %s\n", x.a[0], y.a[1], d, n, s);
    va_end(ap);
}

static void h(const char *s, ...)
{
    va_list *ap_array[3], **ap_ptr = ap_array;
    ap_array[0] = malloc(sizeof(va_list));
    ap_array[1] = NULL;
    ap_array[2] = malloc(sizeof(va_list));
    va_start(*ap_array[0], s);
    vprintf(s, **ap_ptr);
    va_end(**ap_ptr++);
    ap_ptr++;
    va_start(*ap_array[2], s);
    vprintf(s, **ap_ptr);
    va_end(**ap_ptr);
    printf("%d\n", (int)(ap_ptr - ap_array));
    free(ap_array[0]);
    free(ap_array[2]);
}

int main(void)
{
    static big x = { "abc" };
    static big y = { "xyz" };
    f(x, "", 42, 'x', 0);
    g(x, y, 2.5, 7, "seven");
    h("%s\n", "hello");
    return 0;
}
