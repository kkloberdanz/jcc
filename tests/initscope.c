/* A declared name is in scope in its own initializer, with its type. */
#include <stdlib.h>
#include <string.h>

struct pair { long a; long b; };

static int size_of_int(void)
{
    int n = sizeof(n);
    return n;
}

int main(void)
{
    struct pair *p = malloc(sizeof(*p));
    char *s = malloc(sizeof(*s) * 8);
    int sum;

    p->a = 10;
    p->b = size_of_int();
    strcpy(s, "abc");
    sum = (int)(p->a + p->b) + (int)strlen(s) + (int)sizeof(*p);
    free(p);
    free(s);
    return sum;
}
