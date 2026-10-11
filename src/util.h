#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

#define UNUSED(X) (void)(X)

/* allocation that cannot fail: out of memory is a fatal error (src/diag.h), reported and unwound like any other */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

#endif /* UTIL_H */
