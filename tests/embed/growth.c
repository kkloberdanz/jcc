/*
 * A host that keeps one compiler and compiles many units must not grow:
 * every path through the library, the error paths above all, has to free
 * what it allocated. Each unit below is compiled in windows of 200; a
 * leak grows the heap by the same amount in every window, while an
 * allocator settling into its arenas (glibc takes a few hundred compiles)
 * grows it by less each time, so the check passes as soon as one window
 * grows by under 32 bytes per compile and fails if six windows never do
 * (a syntax error used to leak over a kilobyte per unit). The heap size
 * comes from the platform's malloc statistics; where there are none the
 * check is skipped.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kcc.h"

/* AddressSanitizer quarantines freed memory, so its heap only grows: nothing to measure */
#if defined(__SANITIZE_ADDRESS__)
#define ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ASAN 1
#endif
#endif

#if defined(ASAN)
static long heap_in_use(void) { return -1; }
#elif defined(__APPLE__)
#include <malloc/malloc.h>
static long heap_in_use(void) { struct mstats m = mstats(); return (long)m.bytes_used; }
#elif defined(__GLIBC__)
#include <malloc.h>
static long heap_in_use(void) { struct mallinfo2 m = mallinfo2(); return (long)m.uordblks; }
#else
static long heap_in_use(void) { return -1; }
#endif

static void on_diag(void *user, enum KccLevel level, const char *file, unsigned long line, unsigned long col, const char *msg) {
    (void)user; (void)level; (void)file; (void)line; (void)col; (void)msg;
}

static const char *const units[] = {
    "a valid unit",
    "#include <stdio.h>\n"
    "struct s { int a; char b[8]; double d; };\n"
    "static struct s t[4];\n"
    "int f(int n) { int i, k = 0; for (i = 0; i < n; i++) k += t[i & 3].a * 2; return k; }\n"
    "int main(void) { printf(\"%d\\n\", f(3)); return 0; }\n",
    "a semantic error", "int main(void) { int x = 1; return x + nope; }\n",
    "a constraint violation", "struct s { int a; };\nint main(void) { struct s v = {1}; return v < v; }\n",
    "a syntax error", "int main(void) { return 1 +; }\n",
    /* errors that unwind a mid-rule action: the parser must free what the action built */
    "a syntax error in an initializer", "int main(void) { int x = ; return 0; }\n",
    "a syntax error after a label", "int main(void) { l: +; }\n",
    "a syntax error in a struct body", "struct g { }; int main(void) { return 0; }\n",
    "a syntax error in an enum body", "enum e { A = }; int main(void) { return 0; }\n",
    "an invalid token", "int main(void) { return 1 @ 2; }\n",
    "a lexical error", "int main(void) { return 1.0e; }\n",
    "a missing include", "#include \"no-such-file.h\"\nint main(void) { return 0; }\n",
    NULL
};

int main(void) {
    enum { WINDOW = 200, WINDOWS = 6, PER_COMPILE = 32 };
    int failed = 0;
    int u;
    Kcc *k = kcc_new(NULL);

    if (!k) {
        printf("FAIL growth: kcc_new\n");
        return 1;
    }
    kcc_set_diag(k, on_diag, NULL);
    if (heap_in_use() < 0) {
        printf("GROWTH skipped: no usable malloc statistics here\n");
        kcc_free(k);
        return 0;
    }
    for (u = 0; units[u]; u += 2) {
        long before = heap_in_use();
        long grew = 0;
        int w;
        for (w = 0; w < WINDOWS; w++) {
            long after;
            int i;
            for (i = 0; i < WINDOW; i++) {
                char *out = NULL;
                size_t len = 0;
                kcc_compile_to_asm(k, "unit.c", units[u + 1], &out, &len);
                free(out);
            }
            after = heap_in_use();
            grew = (after - before) / WINDOW;
            before = after;
            if (grew < PER_COMPILE) {
                break;
            }
        }
        if (grew >= PER_COMPILE) {
            printf("FAIL growth: %s leaks %ld bytes per compile\n", units[u], grew);
            failed++;
        }
    }
    kcc_free(k);
    printf("GROWTH: %d failed\n", failed);
    return failed != 0;
}
