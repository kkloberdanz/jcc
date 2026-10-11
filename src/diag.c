#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "diag.h"

static int nerrors = 0;
static int nwarnings = 0;
static DiagHandler handler = NULL;
static void *handler_user = NULL;
static jmp_buf *recovery = NULL;

static const char *const level_names[] = { "note", "warning", "error", "fatal error" };

/* the default handler: what the command line compiler prints */
static void print_diag(void *user, DiagLevel level, const char *file, size_t line, size_t col, const char *message) {
    (void)user;
    fflush(stdout);
    if (file) {
        fprintf(stderr, "%s:%lu:%lu: ", file, (unsigned long)line, (unsigned long)col);
    }
    fprintf(stderr, "%s: %s\n", level_names[level], message);
}

void diag_set_handler(DiagHandler fn, void *user) {
    handler = fn;
    handler_user = user;
}

jmp_buf *diag_set_recovery(jmp_buf *where) {
    jmp_buf *previous = recovery;
    recovery = where;
    return previous;
}

static void report(Loc loc, DiagLevel level, const char *fmt, va_list ap) {
    char message[2048];
    vsnprintf(message, sizeof(message), fmt, ap);
    if (loc.file) {
        (handler ? handler : print_diag)(handler_user, level, loc.file, loc.line, loc.col + 1, message);
    } else {
        (handler ? handler : print_diag)(handler_user, level, NULL, 0, 0, message);
    }
}

void diag_error(Loc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    report(loc, DIAG_ERROR, fmt, ap);
    va_end(ap);
    nerrors++;
}

void diag_warning(Loc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    report(loc, DIAG_WARNING, fmt, ap);
    va_end(ap);
    nwarnings++;
}

void diag_note(Loc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    report(loc, DIAG_NOTE, fmt, ap);
    va_end(ap);
}

static void bail(void) {
    nerrors++;
    if (recovery) {
        longjmp(*recovery, 1);
    }
    abort();
}

void diag_fatal(Loc loc, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    report(loc, DIAG_FATAL, fmt, ap);
    va_end(ap);
    bail();
}

void fatal(const char *fmt, ...) {
    Loc none;
    va_list ap;
    none.file = NULL;
    none.line = 0;
    none.col = 0;
    va_start(ap, fmt);
    report(none, DIAG_FATAL, fmt, ap);
    va_end(ap);
    bail();
}

int diag_error_count(void) {
    return nerrors;
}

int diag_warning_count(void) {
    return nwarnings;
}

void diag_reset(void) {
    nerrors = 0;
    nwarnings = 0;
}
