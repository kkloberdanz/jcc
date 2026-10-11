/* A flexible array member (C99, but the Linux headers have one): the last
   member of a struct, with no size of its own. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct packet {
    int len;
    short kind;
    char data[];
};

struct aligned {
    char tag;
    long values[];
};

static struct packet *make(const char *s)
{
    struct packet *p = malloc(sizeof(struct packet) + strlen(s) + 1);
    p->len = (int)strlen(s);
    p->kind = 1;
    strcpy(p->data, s);
    return p;
}

int main(void)
{
    struct packet *p = make("hello, world");
    struct aligned *a = malloc(sizeof(struct aligned) + 3 * sizeof(long));
    int total = 0;

    a->tag = 'x';
    a->values[0] = 10;
    a->values[1] = 20;
    a->values[2] = 30;
    printf("%d %d %d\n", (int)sizeof(struct packet), (int)sizeof(struct aligned), (int)sizeof p->data[0]);
    printf("%s %d %c\n", p->data, p->len, a->tag);
    total = (int)(a->values[0] + a->values[1] + a->values[2]) + p->len + p->data[7];
    free(p);
    free(a);
    return total % 256;
}
