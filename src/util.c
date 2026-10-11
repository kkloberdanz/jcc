#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "diag.h"

void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) {
        fatal("out of memory");
    }
    return p;
}

void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n, size);
    if (!p) {
        fatal("out of memory");
    }
    return p;
}

void *xrealloc(void *p, size_t n) {
    p = realloc(p, n);
    if (!p) {
        fatal("out of memory");
    }
    return p;
}

char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}
