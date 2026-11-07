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

/*
 * The text of a file-scope basic asm (`__asm__("...");`, an N_ATTRIBUTE
 * at file scope), decoded; NULL, with a diagnostic, if its operand is not
 * plain string literals. The caller frees it.
 */
char *sema_file_asm(const AST *item);
/* the text of a basic asm, from its token's (`asm volatile ("..." "...")`), decoded; NULL after an error */
char *sema_asm_text(const char *text, Loc loc);

/* `e` names a bit-field member */
int sema_is_bitfield(const AST *e);
/* `v` reduced to the width and signedness of integer type `t` */
long sema_fit(long v, const Type *t);

/*
 * The atomic builtins: GCC's __atomic_* and __sync_*, and clang's
 * __c11_atomic_* (behind its <stdatomic.h>). Each is type generic: its
 * first operand points to the object, of 1, 2, 4 or 8 bytes. `op` is the
 * helper an operation uses (see AtomicHelper in src/arch.h).
 */
typedef enum AtomicKind {
    AK_LOAD_N,      /* (p, order) -> *p */
    AK_LOAD,        /* (p, ret, order): *ret = *p */
    AK_STORE_N,     /* (p, v, order) */
    AK_STORE,       /* (p, valp, order) */
    AK_INIT,        /* (p, v): a plain store */
    AK_XCHG_N,      /* (p, v, order) -> the old value */
    AK_XCHG,        /* (p, valp, retp, order) */
    AK_CAS_N,       /* (p, expected *, desired, weak, order, order) -> _Bool; clang's has no weak */
    AK_CAS,         /* (p, expected *, desired *, weak, order, order) -> _Bool */
    AK_FETCH,       /* (p, v, order...) -> the old value (or the new, `new_value`) */
    AK_SYNC_BOOL_CAS,   /* (p, old, new) -> _Bool */
    AK_SYNC_VAL_CAS,    /* (p, old, new) -> the old value */
    AK_TAS,         /* (p, order) -> _Bool */
    AK_CLEAR,       /* (p, order) */
    AK_RELEASE,     /* __sync_lock_release(p): stores 0 */
    AK_FENCE,       /* (order) */
    AK_LOCK_FREE    /* (size, p) or (size) -> _Bool */
} AtomicKind;

typedef struct AtomicBuiltin {
    const char *name;
    AtomicKind kind;
    const char *op;     /* AK_FETCH: add, sub, and, or, xor or nand */
    int new_value;      /* AK_FETCH: returns the new value (add_fetch, __sync_add_and_fetch) */
    int nargs;          /* the operands it takes, at least; __sync_* may have more */
    int scaled;         /* clang's fetch_add and fetch_sub of a pointer count elements, not bytes */
} AtomicBuiltin;

/*
 * An asm operand's constraint, its first alternative read for what jcc
 * can give: a register (of the target's general or floating point ones,
 * or the one an x86 letter names), memory, or a constant.
 */
typedef struct AsmConstraint {
    int output;         /* = or + */
    int inout;          /* + */
    int reg;            /* a register will do */
    int mem;            /* memory will do */
    int imm;            /* a constant will do */
    int fp;             /* the register is a floating point one (x86 x, arm64 w) */
    char specific;      /* x86: the register a, b, c, d, S or D names, or the x87 stack's top t or next u; 0 for any */
    int match;          /* an input that must be in output `match`'s place, else -1 */
    const char *match_name; /* [name] in place of the number: the start of it, `match_len` long */
    size_t match_len;
    int unsupported;    /* x87 (t, u, f) or another letter jcc has no place for: the letter */
} AsmConstraint;

/* `text` is the constraint's characters, quotes gone */
void sema_asm_constraint(const char *text, AsmConstraint *out);

/* `&&a - &&b`: the two labels (GNU label arithmetic in a static initializer) */
int sema_label_diff(const AST *e, Symbol **a, Symbol **b);

const AtomicBuiltin *sema_atomic_builtin(const char *name);

#endif /* SEMA_H */
