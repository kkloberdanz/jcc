#ifndef SEMA_H
#define SEMA_H

#include "ast.h"
#include "symtab.h"

/*
 * Semantic analysis: resolves names, gives every expression, declared
 * name and member a type, lays out structs, evaluates constant
 * expressions where C requires one, and inserts ImplicitCast nodes for
 * the conversions C applies silently (array and function decay, the
 * usual arithmetic conversions, assignment conversions).
 *
 * Diagnostics go to stderr (src/diag.h). Returns the number of errors.
 */
int sema(AST *tu, SymTab *symtab);

/*
 * Evaluate an integer constant expression that sema has already typed.
 * Returns 1 and stores the value on success.
 */
int sema_const_eval(AST *expr, long *value);
/* the same for a floating (or integer) constant expression */
int sema_const_eval_float(AST *expr, long double *value);
/* a complex constant expression: its real and imaginary parts */
int sema_const_eval_complex(AST *expr, long double *re, long double *im);

/*
 * The bytes a string literal denotes, decoded, with a terminating NUL.
 * Returns the length without the NUL; the caller frees `*out`.
 */
size_t sema_string_bytes(const char *text, char **out);    /* bytes before the terminator */
size_t sema_string_width(const char *text);                /* bytes per element: 1, or wchar_t's for L"..." */

/* `e` names a bit-field member */
int sema_is_bitfield(const AST *e);
/* `v` reduced to the width and signedness of integer type `t` */
long sema_fit(long v, const Type *t);

#endif /* SEMA_H */
