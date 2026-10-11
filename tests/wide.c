/* Wide string literals: L"..." is an array of wchar_t, which is an int on
   every target; L'x' is an int. */
#include <stdio.h>
#include <stddef.h>
#include <string.h>

static wchar_t w[] = L"abc";
static const wchar_t *p = L"xy" L"z";

static int f(char *x)
{
    return (int)strlen(x);
}

int main(void)
{
    wchar_t local[] = L"hi\x41";
    wchar_t short_init[2] = L"q";
    printf("%d %d %d\n", (int)sizeof w, (int)sizeof(L"abc"), (int)sizeof local);
    printf("%d %d %d %d\n", (int)w[0], (int)w[2], (int)w[3], (int)local[2]);
    printf("%d %d %d\n", (int)p[2], (int)p[3], (int)short_init[1]);
    printf("%d %d\n", f((char *)&L"abcdef"[0]), (int)sizeof(L'a'));
    printf("%d %d\n", L'a' == 'a', (int)L'\x1234');
    return 0;
}
