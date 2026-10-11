#include <stdio.h>
#include <string.h>

#include "target.h"

const Target *const targets[] = {
    &target_arm64_macos,
    &target_x86_64_linux,
    NULL
};

const Target *target = NULL;
const ArchInfo *arch_info = NULL;
const Backend *cg = NULL;

/* the target this compiler was built for, which is what it compiles for by default */
const Target *target_host(void) {
#if defined(__APPLE__) && defined(__aarch64__)
    return &target_arm64_macos;
#elif defined(__linux__) && defined(__x86_64__)
    return &target_x86_64_linux;
#elif defined(__linux__) && defined(__aarch64__)
    return &target_x86_64_linux;    /* no arm64 Linux backend: a cross compiler by default */
#else
#error "kcc has no backend for this host"
#endif
}

static const Target *lookup(const char *name) {
    const Target *const *t;
    const char *const *a;
    for (t = targets; *t; t++) {
        if (strcmp(name, (*t)->name) == 0) {
            return *t;
        }
        for (a = (*t)->aliases; *a; a++) {
            if (strcmp(name, *a) == 0) {
                return *t;
            }
        }
    }
    return NULL;
}

int target_select(const char *name) {
    const Target *t = name ? lookup(name) : target_host();
    if (!t) {
        return 0;
    }
    target = t;
    arch_info = t->info;
    cg = t->cg;
    return 1;
}

int target_is_cross(void) {
    return target != target_host();
}

void target_list(FILE *out) {
    const Target *const *t;
    const char *const *a;
    for (t = targets; *t; t++) {
        fprintf(out, "  %s%s:", (*t)->name, *t == target_host() ? " (host)" : "");
        for (a = (*t)->aliases; *a; a++) {
            fprintf(out, " %s", *a);
        }
        fputc('\n', out);
    }
}
