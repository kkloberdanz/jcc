#ifndef LOWER_H
#define LOWER_H

#include <stddef.h>

#include "arch.h"
#include "ast.h"
#include "symtab.h"
#include "type.h"

/*
 * The two halves of code generation and what each needs from the other.
 * src/codegen.c drives a translation unit: which functions are emitted,
 * static data from constant initializers, string literals, the record
 * classification the ABI needs, and the initializer walker that both
 * static and local initializers go through. src/lower.c turns one
 * function at a time into IR (src/ir.h) and hands it to the backend.
 */

/* lower.c */
struct IRFunc *lower_function(AST *fdef);                      /* its IR, verified, or NULL after errors */
void lower_emit(struct IRFunc *f);                              /* select instructions for it, print it, and free it */
/* the initializer walker's local cases: `off` is a frame offset */
void lower_init_scalar(long off, Type *t, AST *e);              /* the object of type `t` at `off` = e (a scalar, or a record copied) */
void lower_init_string(long off, AST *s, size_t n);             /* `n` bytes of the string literal to the array at `off` */
void lower_init_bitfield(long off, Member *m, AST *item);       /* the bit-field `m` of the unit at `off` = item */
void lower_ldouble_bytes(long double v, unsigned char out[16]); /* the x87 80 bit encoding, little endian in 16 bytes */
void lower_f128_bytes(long double v, unsigned char out[16]);    /* the IEEE binary128 encoding (_Float128), little endian */
void lower_bid_bytes(const char *text, int neg, int kind, unsigned char out[16]);   /* a decimal constant's BID encoding */
int lower_check_value_type(Loc loc, const Type *t);            /* 0 after a diagnostic when values of `t` are not supported yet */

/* codegen.c */
extern int codegen_dump_ir;                                     /* -fdump-ir: print each function's IR to stdout */
extern int codegen_opt_level;                                   /* -O: 0, or 1 to optimize (src/opt.h) */
int codegen_add_string(AST *s);                                 /* the label of a string literal, emitted after the code */
void codegen_local_initializer(Type *t, AST *init, long off);   /* walk a local object's initializer at `off` */
void codegen_static_local(Symbol *s);                           /* a static local: data, with a unique name */
void codegen_arg_type(Type *t, ArgType *at);                    /* what the ABI needs to know about a value of type `t` */
const char *codegen_sym_name(const Symbol *s);                  /* the undecorated assembler name */
void codegen_use_atomic(const char *op, int size);
int lower_uses_va_arg_pack(AST *fdef);   /* the function passes its extra arguments on: inlined at each call, never on its own */
extern int lower_init_reverse;      /* initializing a member of a struct in the reversed byte order (scalar_storage_order) */
extern int codegen_user_asm;    /* the unit has an asm statement with instructions of its own (not just an empty template) */
int codegen_label_id(Symbol *label);
int codegen_new_label_id(void);                                   /* the same, for a block that is no C label */                             /* the unit-wide number of a label whose address is taken */              /* the unit calls __jcc_atomic_<op>_<size> (src/arch.h), emitted at its end */
int codegen_sym_defined_here(const Symbol *s);
int codegen_sym_is_local(const Symbol *s);                      /* an automatic variable or a parameter */
int codegen_sym_is_static_local(const Symbol *s);

#endif /* LOWER_H */
