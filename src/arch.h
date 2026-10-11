#ifndef ARCH_H
#define ARCH_H

#include <stddef.h>
#include <stdio.h>

/* the compiler is written for LP64 hosts: `long` carries every 64 bit target value */
typedef char kcc_requires_64_bit_long[sizeof(long) == 8 ? 1 : -1];

/*
 * What the front end needs to know about the target: the width and
 * alignment of every basic type. Everything else about a type (struct
 * layout, promotions) is derived from these in src/type.c.
 */
typedef struct ArchInfo {
    const char *name;
    int char_is_signed;
    size_t short_size;
    size_t int_size;
    size_t long_size;
    size_t llong_size;
    size_t int128_size;
    size_t int128_align;
    size_t ptr_size;
    size_t float_size;
    size_t double_size;
    size_t ldouble_size;
    size_t ldouble_align;
    size_t va_list_size;
    size_t va_list_align;
    int va_list_is_array;   /* va_list is an array type: decays to a pointer, parameters are pointers */
    int ldouble_x87;        /* a 16 byte long double is the x87 80 bit format, which the backend's 16 byte floating primitives implement */
    int fp_round;           /* the backend has IR_FRINT: floor, ceil, trunc and round are instructions at -O1 */
} ArchInfo;

/*
 * The code generation interface. A backend emits static data from the
 * primitives below, decides where a call's arguments go (the ABI
 * allocator, which the lowering also uses for a function's parameters),
 * and turns one function's IR (src/ir.h) into assembly in `select`:
 * instruction selection over virtual registers, whose places the
 * allocator (src/regalloc.c) chose, with the prologue and epilogue
 * emitted around the body once the frame size is known.
 */

struct IRFunc;

/*
 * Argument passing. The backend assigns each argument of a call, or each
 * parameter of a function, to registers or to a byte range in the
 * outgoing stack area, following the platform ABI (System V uses 8 byte
 * slots; Apple arm64 packs scalars at their natural size). A record
 * passed by reference is copied into the area after the real arguments
 * and its address is passed in the argument's place.
 */
typedef enum ArgKind {
    ARG_INT,            /* an integer or pointer, up to 8 bytes */
    ARG_FLOAT,          /* a float or double */
    ARG_RECORD,         /* a struct or union, passed by value */
    ARG_RECORD_MEMORY,  /* one with a member off its natural alignment (packed): System V passes it in memory */
    ARG_CX87            /* a _Complex long double where long double is x87: in memory, returned in st(0) and st(1) */
} ArgKind;

/*
 * What the ABI needs to know about an argument's type. A record's
 * eightbytes are classified by codegen from the member types, so that
 * the backends only apply their rules: an x86_64 eightbyte holding
 * nothing but floating point is class SSE and travels in a vector
 * register; an arm64 record of one to four floating members of one
 * type is a homogeneous floating point aggregate (HFA) and travels in
 * consecutive vector registers.
 */
typedef struct ArgType {
    size_t size;
    size_t align;
    ArgKind kind;       /* ARG_INT with size 16 is an __int128 */
    size_t hfa;         /* HFA: the member count, else 0 */
    size_t hfa_size;    /* HFA: each member's size, 4 or 8 */
    unsigned sse;       /* bit k set: eightbyte k holds only floating point */
} ArgType;

typedef struct ArgLoc {
    int in_regs;        /* in registers, otherwise the bytes offset .. offset+size-1 of the area */
    int in_fregs;       /* with in_regs, a scalar: the register is a floating point one */
    int by_ref;         /* a record: the copy is at byte `copy` of the area, and a pointer to it is what is passed */
    size_t reg;         /* the first integer register, and the first floating point one; a record's */
    size_t freg;        /*   eightbytes (or HFA members) take them in order according to its ArgType */
    size_t nregs;       /* the values pushed for it: eightbytes, HFA members, or 2 for an __int128 */
    size_t offset;
    size_t size;
    size_t copy;
    size_t copy_size;   /* by_ref: the bytes the copy takes */
    ArgType type;
} ArgLoc;

typedef struct ArgAlloc {
    size_t nregs;       /* integer argument registers used so far */
    size_t nfregs;      /* floating point argument registers used so far */
    size_t nstack;      /* bytes of the stack area used so far */
    size_t nfixed;      /* named parameters; the rest are variadic */
    size_t index;       /* arguments assigned so far */
} ArgAlloc;

/*
 * The primitives, one table per backend. The selected one is `cg`; see
 * src/target.c.
 */
typedef struct Backend {
    /* the whole file */
    void (*begin)(FILE *out);
    void (*end)(void);

    /* initialized and zero data; `name` is the C name, the backend decorates it */
    void (*data_begin)(const char *name, int is_global, size_t align, int zero);
    void (*data_int)(size_t size, long value);
    void (*data_addr)(const char *name, long addend);
    void (*data_string_addr)(int label, long addend);
    void (*data_bytes)(const char *bytes, size_t len);
    void (*data_zero)(size_t size);
    /* a string literal in read-only data; `len` includes the terminating NUL */
    void (*string)(int label, const char *bytes, size_t len);

    /* frame offset of the argument that arrived at byte `offset` of the caller's stack area */
    long (*stack_param_offset)(size_t offset);

    /* `sret` when the callee returns a record through a hidden pointer argument */
    void (*args_begin)(ArgAlloc *a, size_t nfixed, int sret);
    /* the next argument */
    void (*args_next)(ArgAlloc *a, const ArgType *t, ArgLoc *loc);
    /* place the by-reference copies; returns the size of the stack area in bytes */
    size_t (*args_end)(ArgAlloc *a, ArgLoc *locs, size_t n);
    /* whether a record is returned in registers rather than through a hidden pointer */
    int (*record_ret_regs)(const ArgType *t);

    /* a whole function: allocate its registers, select instructions, print it */
    void (*select)(struct IRFunc *f);
} Backend;

/* the selected target's type sizes and primitives; target_select sets them */
extern const ArchInfo *arch_info;
extern const Backend *cg;

#endif /* ARCH_H */
