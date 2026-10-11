#ifndef TYPE_H
#define TYPE_H

#include <stddef.h>
#include <stdio.h>

/*
 * Type specifier keywords as they appear in a declaration, before they are
 * combined into a type (`unsigned long` is two of these).
 */
typedef enum TypeSpecKind {
    TS_VOID,
    TS_CHAR,
    TS_SHORT,
    TS_INT,
    TS_LONG,
    TS_FLOAT,
    TS_DOUBLE,
    TS_SIGNED,
    TS_UNSIGNED,
    TS_BUILTIN_VA_LIST,
    TS_INT128,
    TS_UINT128,
    TS_BOOL,
    TS_FLOAT16,
    TS_COMPLEX
} TypeSpecKind;

/*
 * Types proper. Every expression, declared name and member gets one of
 * these after semantic analysis.
 *
 * Types are not interned: two `int *` built separately are two objects,
 * and equality is structural (type_compatible). Struct, union and enum
 * types share one Record per tag, so completing a struct completes every
 * pointer to it. All types are released at once by type_free_all.
 */
#define TYPE_KINDS(X) \
    X(TY_VOID, "void") \
    X(TY_BOOL, "_Bool") \
    X(TY_CHAR, "char") \
    X(TY_SCHAR, "signed char") \
    X(TY_UCHAR, "unsigned char") \
    X(TY_SHORT, "short") \
    X(TY_USHORT, "unsigned short") \
    X(TY_INT, "int") \
    X(TY_UINT, "unsigned int") \
    X(TY_LONG, "long") \
    X(TY_ULONG, "unsigned long") \
    X(TY_LLONG, "long long") \
    X(TY_ULLONG, "unsigned long long") \
    X(TY_INT128, "__int128") \
    X(TY_UINT128, "unsigned __int128") \
    X(TY_FLOAT16, "_Float16") \
    X(TY_FLOAT, "float") \
    X(TY_DOUBLE, "double") \
    X(TY_LDOUBLE, "long double") \
    X(TY_CFLOAT, "float _Complex") \
    X(TY_CDOUBLE, "double _Complex") \
    X(TY_CLDOUBLE, "long double _Complex") \
    X(TY_VA_LIST, "__builtin_va_list") \
    X(TY_ENUM, "enum") \
    X(TY_STRUCT, "struct") \
    X(TY_UNION, "union") \
    X(TY_POINTER, "pointer") \
    X(TY_ARRAY, "array") \
    X(TY_FUNC, "function")

typedef enum TypeKind {
#define X(name, repr) name,
    TYPE_KINDS(X)
#undef X
    TY_NKINDS
} TypeKind;

/* qualifier bits */
#define TQ_CONST 1u
#define TQ_VOLATILE 2u

typedef struct Type Type;
typedef struct Record Record;
typedef struct Member Member;
typedef struct Param Param;
struct Symbol;  /* src/symtab.h */
struct AST;     /* src/ast.h */

struct Member {
    char *name;         /* NULL for an unnamed bit-field */
    Type *type;
    size_t offset;      /* bytes from the start of the struct */
    int bits;           /* bit-field width, or -1 */
    unsigned bit_offset;/* bit-field: first bit within the unit at `offset` */
    size_t unit;        /* bit-field: bytes of the unit at `offset` that holds it */
    struct AST *decl;   /* the StructDeclarator */
};

/* a struct, union or enum: the thing a tag names */
struct Record {
    TypeKind kind;      /* TY_STRUCT, TY_UNION or TY_ENUM */
    char *tag;          /* NULL when anonymous */
    int complete;       /* body has been seen */
    Member *members;    /* struct and union only */
    size_t nmembers;
    size_t size;
    size_t align;
    int packed;         /* __attribute__((packed)) */
    size_t pack;        /* `#pragma pack(n)` in effect at the definition, 0 if none */
    int is_unsigned;    /* enum: no enumerator is negative, so the type is unsigned int */
    Type *underlying;   /* enum: the fixed underlying type (`enum : T`), NULL for the usual int */
    size_t aligned;     /* __attribute__((aligned(n))), 0 if none */
    struct Symbol *sym; /* the tag symbol, NULL when anonymous */
    Record *next_alloc;
};

struct Param {
    char *name;         /* may be NULL in a prototype */
    Type *type;
};

struct Type {
    TypeKind kind;
    unsigned quals;     /* TQ_* bits */
    Type *base;         /* pointer target, array element, function return */
    long length;        /* array: element count, -1 when incomplete */
    Record *record;     /* struct, union, enum */
    Param *params;      /* function */
    size_t nparams;
    int variadic;
    int has_proto;      /* function: declared with a parameter type list */
    Type *next_alloc;
};

/* construction; every type returned here is owned by the arena */

Type *type_basic(TypeKind kind);          /* shared, unqualified singleton */
Type *type_pointer(Type *base);
Type *type_array(Type *elem, long length);
Type *type_func(Type *ret, Param *params, size_t nparams, int variadic, int has_proto);
Type *type_record(Record *record);
/* a copy of `t` with `quals` added; `t` itself when nothing changes */
Type *type_qualify(Type *t, unsigned quals);
Type *type_unqualified(Type *t);
Record *record_new(TypeKind kind, const char *tag);
/* `name` is copied */
void record_add_member(Record *r, const char *name, Type *type, int bits, struct AST *decl);
Member *record_find(const Record *r, const char *name);
/* find a member by name, also inside anonymous struct and union members (C11; a common extension before it) */
#define RECORD_PATH_MAX 8
Member *record_lookup(const Record *r, const char *name, Member **path, size_t *depth);
/* whether some member sits at an offset that is not a multiple of its alignment (packed), also in nested records */
int record_has_unaligned(const Record *r);
/* compute offsets, size and alignment; marks the record complete */
void record_layout(Record *r);
void type_free_all(void);

/* classification */

int type_is_integer(const Type *t);     /* char..int128, enum */
int type_is_unsigned(const Type *t);
int type_is_floating(const Type *t);
int type_is_complex(const Type *t);       /* float, double or long double _Complex */
int type_is_wide(const Type *t);          /* __int128, either sign */
int type_is_arithmetic(const Type *t);
int type_is_scalar(const Type *t);      /* arithmetic or pointer */
int type_is_pointer(const Type *t);
int type_is_record(const Type *t);      /* struct or union */
int type_is_complete(const Type *t);
int type_is_void(const Type *t);
int type_is_void_pointer(const Type *t);
int type_is_func_pointer(const Type *t);
/* integer conversion rank, higher wins in the usual arithmetic conversions */
int type_rank(const Type *t);

/* sizes, from the target description in src/arch.h */

size_t type_size(const Type *t);
size_t type_align(const Type *t);
/* the integer type sizeof yields, and the one pointer subtraction yields */
Type *type_size_t(void);
Type *type_ptrdiff_t(void);

/* conversions */

Type *type_promote(Type *t);                    /* integer promotions */
Type *type_arith_common(Type *a, Type *b);      /* usual arithmetic conversions */
Type *type_complex_base(const Type *t);         /* the component type of a complex type */
Type *type_complex_of(const Type *real);        /* the complex type with that component type */
/* array of T -> pointer to T, function -> pointer to function, else `t` */
Type *type_decay(Type *t);

/* comparison */

int type_compatible(const Type *a, const Type *b);
/* like type_compatible but ignoring the outermost qualifiers */
int type_compatible_unqual(const Type *a, const Type *b);
/* the more complete of two compatible types (array length, prototype) */
Type *type_composite(Type *a, Type *b);

/* printing: C syntax, e.g. `int (*)(const char *, ...)`; caller frees */
char *type_to_string(const Type *t);
void type_print(const Type *t, FILE *out);
const char *type_kind_name(TypeKind kind);

/* the bit pattern of `v` as a value of floating type `t` (float or double) */
long type_float_bits(long double v, const Type *t);

#endif /* TYPE_H */
