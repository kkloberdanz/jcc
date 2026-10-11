#ifndef DIAG_H
#define DIAG_H

#include <setjmp.h>
#include <stddef.h>

#include "ast.h"
#include "kcc.h"

/*
 * Every message the compiler produces goes through here. By default they
 * are printed to stderr as `file:line:col: error: message`, with the
 * column counted from 1 as editors do (the dumps count from 0). A host
 * that links the compiler in installs a handler instead and sees each
 * one as file, line, column, level and text.
 */
/* the levels and the handler type are the public ones (src/kcc.h) */
typedef KccLevel DiagLevel;
#define DIAG_NOTE KCC_NOTE
#define DIAG_WARNING KCC_WARNING
#define DIAG_ERROR KCC_ERROR
#define DIAG_FATAL KCC_FATAL
typedef KccDiagFn DiagHandler;

/* install a handler; NULL restores printing to stderr */
void diag_set_handler(DiagHandler handler, void *user);

void diag_error(Loc loc, const char *fmt, ...);
void diag_warning(Loc loc, const char *fmt, ...);
void diag_note(Loc loc, const char *fmt, ...);

/*
 * A fatal error reports the message and abandons the compile: control
 * returns to the recovery point the entry function set with
 * diag_set_recovery (the memory allocated since is leaked). Without a
 * recovery point the process aborts, which is a bug in the caller.
 */
void diag_fatal(Loc loc, const char *fmt, ...);
void fatal(const char *fmt, ...);               /* the same, without a location */

/* install the jmp_buf a fatal error returns to (NULL: none); returns the previous one, to restore */
jmp_buf *diag_set_recovery(jmp_buf *where);

int diag_error_count(void);
int diag_warning_count(void);
void diag_reset(void);

#endif /* DIAG_H */
