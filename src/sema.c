#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "sema.h"
#include "diag.h"
#include "type.h"
#include "arch.h"
#include "util.h"

/*
 * Layout of this file:
 *   - helpers (type names for messages, literal decoding)
 *   - constant expressions
 *   - specifiers and declarators to types
 *   - expressions
 *   - initializers
 *   - statements, functions, the translation unit
 *   - builtin functions
 */

static SymTab *cur_symtab = NULL;

/* the function being checked */
static Type *cur_return = NULL;
static ASTList *cur_gotos = NULL;
static int loop_depth = 0;

typedef struct SwitchCtx {
    Type *type;
    long *values;
    size_t nvalues;
    size_t cap;
    int has_default;
    struct SwitchCtx *outer;
} SwitchCtx;

static SwitchCtx *switch_ctx = NULL;

/* a specifier list, combined */
typedef struct Specs {
    Type *type;
    StorageKind storage;
    int is_inline;
    int is_typedef;
    int packed;         /* __attribute__((packed)) seen */
    size_t aligned;     /* __attribute__((aligned(n))) seen */
    int only_tag;       /* the list is just `struct S` or `enum E` (with or without body) */
} Specs;

static Type *sema_expr(AST **slot);
static Type *sema_rvalue(AST **slot);
static void sema_specs(ASTList *specs, Specs *out, int allow_storage, Loc loc);
static Type *sema_declarator(AST *d, Type *base, const char **name_out);
static Type *sema_type_name(AST *tn);
static void sema_declaration(AST *decl, int file_scope);
static void sema_stmt(AST *stmt);
static void sema_initializer(AST **slot, Type **ptype, int is_static);
static int float_eval(AST *e, long double *out);
static Type *builtin_type(const char *name);
static Type *sema_atomic(AST *e, int is_store);

/* helpers */

/* a type's spelling for a message; the buffers rotate so one message can name four types */
static const char *ts(const Type *t) {
    static char bufs[4][160];
    static int next = 0;
    char *buf = bufs[next];
    char *s = type_to_string(t);
    next = (next + 1) % 4;
    snprintf(buf, sizeof(bufs[0]), "%s", s);
    free(s);
    return buf;
}

static Type *int_type(void) {
    return type_basic(TY_INT);
}

/*
 * The type an erroneous expression gets, so that one error does not
 * cascade. `lvalue` is set to -1 to mark the node as already diagnosed.
 */
static Type *error_type(AST *e) {
    e->type = int_type();
    e->lvalue = -1;
    return e->type;
}

static int poisoned(const AST *e) {
    return e->lvalue < 0;
}

int sema_is_bitfield(const AST *e) {
    return e->kind == N_MEMBER && e->u.member.member && e->u.member.member->bits >= 0;
}

static int is_static_storage(const Symbol *sym) {
    if (sym->kind == SYM_FUNCTION) {
        return 1;
    }
    if (sym->kind != SYM_VARIABLE) {
        return 0;
    }
    return sym->scope->kind == SCOPE_FILE
        || sym->storage == STORAGE_STATIC
        || sym->storage == STORAGE_EXTERN;
}

/* decode one escape sequence at *p (just after the backslash); returns its value */
static long decode_escape(const char **p) {
    const char *s = *p;
    long v = 0;
    int n;

    switch (*s) {
        case 'n': *p = s + 1; return '\n';
        case 't': *p = s + 1; return '\t';
        case 'r': *p = s + 1; return '\r';
        case 'a': *p = s + 1; return '\a';
        case 'b': *p = s + 1; return '\b';
        case 'f': *p = s + 1; return '\f';
        case 'v': *p = s + 1; return '\v';
        case 'e': *p = s + 1; return 27;
        case 'x':
            s++;
            while ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F')) {
                int d = *s <= '9' ? *s - '0' : (*s | 0x20) - 'a' + 10;
                v = v * 16 + d;
                s++;
            }
            *p = s;
            return v;
        default:
            if (*s >= '0' && *s <= '7') {
                for (n = 0; n < 3 && *s >= '0' && *s <= '7'; n++) {
                    v = v * 8 + (*s - '0');
                    s++;
                }
                *p = s;
                return v;
            }
            *p = s + 1;
            return (unsigned char)*s;
    }
}

/* a wide literal: L"..." or L'.' (any piece of a concatenation) */
static int literal_is_wide(const char *text) {
    const char *p = text;
    for (;;) {
        char quote;
        while (*p == ' ') {
            p++;
        }
        if (*p == 'L') {
            return 1;
        }
        if (*p != '"' && *p != '\'') {
            return 0;
        }
        quote = *p++;
        while (*p && *p != quote) {
            if (*p == '\\' && p[1]) {
                p++;
            }
            p++;
        }
        if (*p) {
            p++;
        }
    }
}

/* the bytes of a wide element: wchar_t is an int on every target, little-endian */
static size_t wide_width(void) {
    return type_size(int_type());
}

/*
 * The elements a string or character literal denotes, without the quotes;
 * `out` (which may be NULL) receives them as `width` byte little-endian
 * units: 1 for a plain literal, wide_width() for a wide one. `text` may be
 * several adjacent literals separated by spaces (ast_string_concat).
 */
static size_t decode_literal(const char *text, long *first, char *out, size_t width) {
    const char *p = text;
    size_t n = 0;
    char quote;
    int wide = literal_is_wide(text);

    for (;;) {
        while (*p == ' ') {
            p++;
        }
        if (*p == 'L') {
            p++;
        }
        if (*p != '"' && *p != '\'') {
            break;
        }
        quote = *p++;
        while (*p && *p != quote) {
            long v;
            if (*p == '\\') {
                p++;
                v = decode_escape(&p);
            } else if (wide && (unsigned char)*p >= 0xC0) {
                /* the source is UTF-8: a wide literal holds the code point */
                unsigned char c = (unsigned char)*p++;
                int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
                v = c & (0x3F >> extra);
                while (extra-- > 0 && ((unsigned char)*p & 0xC0) == 0x80) {
                    v = (v << 6) | ((unsigned char)*p++ & 0x3F);
                }
            } else {
                v = (unsigned char)*p++;
            }
            if (n == 0 && first) {
                *first = v;
            }
            if (out) {
                size_t k;
                for (k = 0; k < width; k++) {
                    out[n * width + k] = (char)(v >> (8 * k));
                }
            }
            n++;
        }
        if (*p == quote) {
            p++;
        }
    }
    return n;
}

static long char_literal_value(const char *text) {
    long first = 0;
    size_t n = decode_literal(text, &first, NULL, 1);
    if (n == 0) {
        return 0;
    }
    /* a plain char constant has the value a char object of that value would have */
    if (!literal_is_wide(text) && arch_info->char_is_signed && first >= 128 && first < 256) {
        first -= 256;
    }
    return first;
}

static unsigned long max_of(TypeKind kind) {
    size_t bits = type_size(type_basic(kind)) * 8;
    int is_unsigned = type_is_unsigned(type_basic(kind));
    if (bits >= 64) {
        return is_unsigned ? ~0ul : ~0ul >> 1;
    }
    return is_unsigned ? (1ul << bits) - 1 : (1ul << (bits - 1)) - 1;
}

/* C89 integer constant typing, extended with long long for the system headers */
static Type *int_literal_type(const char *text, unsigned long *value) {
    const char *p = text;
    int has_u = 0;
    int nl = 0;
    int decimal;
    unsigned long v;
    TypeKind candidates[6];
    int n = 0;
    int i;

    errno = 0;
    v = strtoul(text, (char **)&p, 0);
    decimal = text[0] != '0';
    while (*p) {
        if (*p == 'u' || *p == 'U') {
            has_u = 1;
        } else if (*p == 'l' || *p == 'L') {
            nl++;
        }
        p++;
    }
    *value = v;
    if (nl >= 2) {
        candidates[n++] = has_u ? TY_ULLONG : TY_LLONG;
        candidates[n++] = TY_ULLONG;
    } else if (nl == 1) {
        if (!has_u) {
            candidates[n++] = TY_LONG;
        }
        candidates[n++] = TY_ULONG;
        candidates[n++] = TY_ULLONG;
    } else if (has_u) {
        candidates[n++] = TY_UINT;
        candidates[n++] = TY_ULONG;
        candidates[n++] = TY_ULLONG;
    } else if (decimal) {
        candidates[n++] = TY_INT;
        candidates[n++] = TY_LONG;
        candidates[n++] = TY_ULONG;
        candidates[n++] = TY_ULLONG;
    } else {
        candidates[n++] = TY_INT;
        candidates[n++] = TY_UINT;
        candidates[n++] = TY_LONG;
        candidates[n++] = TY_ULONG;
        candidates[n++] = TY_ULLONG;
    }
    for (i = 0; i < n; i++) {
        if (v <= max_of(candidates[i])) {
            return type_basic(candidates[i]);
        }
    }
    return type_basic(candidates[n - 1]);
}

/* the suffix letters of a floating constant, in any order: f, l, and i or j for an imaginary one */
static void float_suffix(const char *text, int *is_float, int *is_long, int *is_imaginary) {
    size_t n = strlen(text);
    *is_float = *is_long = *is_imaginary = 0;
    while (n > 0 && isalpha((unsigned char)text[n - 1]) && !(text[n - 1] == 'p' || text[n - 1] == 'P')) {
        char c = (char)tolower((unsigned char)text[n - 1]);
        if (c == 'f') {
            *is_float = 1;
        } else if (c == 'l') {
            *is_long = 1;
        } else if (c == 'i' || c == 'j') {
            *is_imaginary = 1;
        } else {
            break;      /* a hexadecimal digit */
        }
        n--;
    }
}

static Type *float_literal_type(const char *text) {
    int is_float, is_long, is_imaginary;
    Type *t;
    float_suffix(text, &is_float, &is_long, &is_imaginary);
    t = type_basic(is_float ? TY_FLOAT : is_long ? TY_LDOUBLE : TY_DOUBLE);
    return is_imaginary ? type_complex_of(t) : t;
}

/* constant expressions */

/* reduce `v` to the width and signedness of `t` */
long sema_fit(long v, const Type *t) {
    size_t bits = type_size(t) * 8;
    unsigned long mask;
    if (bits >= 64 || bits == 0) {
        return v;
    }
    mask = (1ul << bits) - 1;
    v &= (long)mask;
    if (!type_is_unsigned(t) && (v & (1l << (bits - 1)))) {
        v |= (long)~mask;
    }
    return v;
}

static int eval_binary(OpKind op, long a, long b, const Type *t, long *out) {
    int u = type_is_unsigned(t);
    unsigned long ua = (unsigned long)a;
    unsigned long ub = (unsigned long)b;

    switch (op) {
        case OP_ADD: *out = (long)(ua + ub); break;
        case OP_SUB: *out = (long)(ua - ub); break;
        case OP_MUL: *out = (long)(ua * ub); break;
        case OP_DIV:
            if (b == 0) {
                return 0;
            }
            *out = u ? (long)(ua / ub) : a / b;
            break;
        case OP_MOD:
            if (b == 0) {
                return 0;
            }
            *out = u ? (long)(ua % ub) : a % b;
            break;
        case OP_SHL:
        case OP_SHR:
            if (type_size(t) > 8 && (b < 0 || b >= 64)) {
                return 0;   /* an __int128 shifted past what fits here: not a constant for us */
            }
            if (op == OP_SHL) {
                *out = (long)(ua << (ub & 63));
            } else {
                *out = u ? (long)(ua >> (ub & 63)) : a >> (ub & 63);
            }
            break;
        case OP_LT: *out = u ? ua < ub : a < b; break;
        case OP_GT: *out = u ? ua > ub : a > b; break;
        case OP_LE: *out = u ? ua <= ub : a <= b; break;
        case OP_GE: *out = u ? ua >= ub : a >= b; break;
        case OP_EQ: *out = a == b; break;
        case OP_NE: *out = a != b; break;
        case OP_BITAND: *out = a & b; break;
        case OP_BITXOR: *out = a ^ b; break;
        case OP_BITOR: *out = a | b; break;
        default:
            return 0;
    }
    return 1;
}

int sema_const_eval(AST *e, long *out) {
    long a;
    long b;

    if (!e || !e->type) {
        return 0;
    }
    switch (e->kind) {
        case N_INT: {
            unsigned long v;
            int_literal_type(e->u.text, &v);
            *out = sema_fit((long)v, e->type);
            return 1;
        }

        case N_CHAR:
            *out = char_literal_value(e->u.text);
            return 1;

        case N_IDENT:
            if (e->sym && e->sym->kind == SYM_ENUMERATOR) {
                *out = e->sym->value;
                return 1;
            }
            return 0;

        case N_UNARY:
            if (!sema_const_eval(e->u.unary.operand, &a)) {
                return 0;
            }
            switch (e->u.unary.op) {
                case OP_POS: *out = a; break;
                case OP_NEG: *out = sema_fit(-a, e->type); break;
                case OP_BITNOT: *out = sema_fit(~a, e->type); break;
                case OP_NOT: *out = !a; break;
                default:
                    return 0;
            }
            return 1;

        case N_BINARY: {
            OpKind op = e->u.binary.op;
            if (op == OP_AND || op == OP_OR) {
                if (!sema_const_eval(e->u.binary.left, &a)) {
                    return 0;
                }
                if (op == OP_AND && !a) {
                    *out = 0;
                    return 1;
                }
                if (op == OP_OR && a) {
                    *out = 1;
                    return 1;
                }
                if (!sema_const_eval(e->u.binary.right, &b)) {
                    return 0;
                }
                *out = b != 0;
                return 1;
            }
            if (!sema_const_eval(e->u.binary.left, &a) || !sema_const_eval(e->u.binary.right, &b)) {
                return 0;
            }
            /* comparisons are done in the operands' common type, which
               both operands were converted to */
            if (!eval_binary(op, a, b, e->u.binary.left->type, out)) {
                return 0;
            }
            *out = sema_fit(*out, e->type);
            return 1;
        }

        case N_COND:
            if (!sema_const_eval(e->u.cond.cond, &a)) {
                return 0;
            }
            return sema_const_eval(a ? e->u.cond.then : e->u.cond.otherwise, out);

        case N_CAST:
        case N_IMPLICIT_CAST: {
            AST *operand = e->u.cast.expr;
            if (type_is_integer(e->type) || e->type->kind == TY_POINTER) {
                if (type_is_floating(operand->type)) {
                    long double d;
                    if (!float_eval(operand, &d)) {
                        return 0;
                    }
                    *out = e->type->kind == TY_BOOL ? d != 0 : sema_fit((long)d, e->type);
                    return 1;
                }
                if (!sema_const_eval(operand, &a)) {
                    return 0;
                }
                *out = e->type->kind == TY_BOOL ? a != 0 : sema_fit(a, e->type);
                return 1;
            }
            return 0;
        }

        case N_SIZEOF_EXPR:
            if (!e->u.cast.expr->type) {
                return 0;
            }
            *out = (long)type_size(e->u.cast.expr->type);
            return 1;

        case N_SIZEOF_TYPE:
            if (!e->u.cast.type->type) {
                return 0;
            }
            *out = (long)type_size(e->u.cast.type->type);
            return 1;

        default:
            return 0;
    }
}

/* floating constant expressions, only needed under a cast to an integer type */
static int float_eval(AST *e, long double *out);

/* the value a float or double node has: rounded to its type, as the generated code would */
static long double round_to_type(long double v, const Type *t) {
    if (t->kind == TY_FLOAT) {
        return (long double)(float)v;
    }
    if (t->kind == TY_DOUBLE) {
        return (long double)(double)v;
    }
    return v;
}

static int float_eval_raw(AST *e, long double *out);

static int float_eval(AST *e, long double *out) {
    if (!float_eval_raw(e, out)) {
        return 0;
    }
    *out = round_to_type(*out, e->type);
    return 1;
}

static int float_eval_raw(AST *e, long double *out) {
    long double a;
    long double b;
    long i;

    if (!e || !e->type) {
        return 0;
    }
    if (type_is_integer(e->type)) {
        if (!sema_const_eval(e, &i)) {
            return 0;
        }
        *out = type_is_unsigned(e->type) ? (long double)(unsigned long)i : (long double)i;
        return 1;
    }
    switch (e->kind) {
        case N_FLOAT:
            if (type_is_complex(e->type)) {
                return 0;
            }
            *out = strtold(e->u.text, NULL);
            return 1;
        case N_UNARY:
            if (!float_eval(e->u.unary.operand, &a)) {
                return 0;
            }
            if (e->u.unary.op == OP_NEG) {
                *out = -a;
            } else if (e->u.unary.op == OP_POS) {
                *out = a;
            } else {
                return 0;
            }
            return 1;
        case N_BINARY:
            if (!float_eval(e->u.binary.left, &a) || !float_eval(e->u.binary.right, &b)) {
                return 0;
            }
            switch (e->u.binary.op) {
                case OP_ADD: *out = a + b; return 1;
                case OP_SUB: *out = a - b; return 1;
                case OP_MUL: *out = a * b; return 1;
                case OP_DIV: *out = a / b; return 1;
                default: return 0;
            }
        case N_COND:
            if (!sema_const_eval(e->u.cond.cond, &i)) {
                return 0;
            }
            return float_eval(i ? e->u.cond.then : e->u.cond.otherwise, out);
        case N_CAST:
        case N_IMPLICIT_CAST:
            return float_eval(e->u.cast.expr, out);
        default:
            return 0;
    }
}

/*
 * A complex constant: an imaginary constant, a real constant (imaginary
 * part 0), and sums, products, negations and casts of them. The parts
 * are rounded to the component type at each step, as float_eval does.
 */
static int complex_eval(AST *e, long double *re, long double *im) {
    long double ar, ai, br, bi;
    long i;
    Type *base;

    if (!e || !e->type) {
        return 0;
    }
    if (!type_is_complex(e->type)) {
        if (!float_eval(e, re)) {
            return 0;
        }
        *im = 0;
        return 1;
    }
    base = type_complex_base(e->type);
    switch (e->kind) {
        case N_FLOAT:
            *re = 0;
            *im = round_to_type(strtold(e->u.text, NULL), base);
            return 1;
        case N_UNARY:
            if (!complex_eval(e->u.unary.operand, &ar, &ai)) {
                return 0;
            }
            if (e->u.unary.op == OP_NEG) {
                *re = -ar;
                *im = -ai;
            } else if (e->u.unary.op == OP_POS) {
                *re = ar;
                *im = ai;
            } else {
                return 0;
            }
            return 1;
        case N_BINARY:
            if (!complex_eval(e->u.binary.left, &ar, &ai) || !complex_eval(e->u.binary.right, &br, &bi)) {
                return 0;
            }
            switch (e->u.binary.op) {
                case OP_ADD: *re = ar + br; *im = ai + bi; break;
                case OP_SUB: *re = ar - br; *im = ai - bi; break;
                case OP_MUL: *re = ar * br - ai * bi; *im = ar * bi + ai * br; break;
                case OP_DIV: {
                    long double d = br * br + bi * bi;
                    *re = (ar * br + ai * bi) / d;
                    *im = (ai * br - ar * bi) / d;
                    break;
                }
                default: return 0;
            }
            *re = round_to_type(*re, base);
            *im = round_to_type(*im, base);
            return 1;
        case N_COND:
            if (!sema_const_eval(e->u.cond.cond, &i)) {
                return 0;
            }
            return complex_eval(i ? e->u.cond.then : e->u.cond.otherwise, re, im);
        case N_CAST:
        case N_IMPLICIT_CAST:
            if (!complex_eval(e->u.cast.expr, &ar, &ai)) {
                return 0;
            }
            *re = round_to_type(ar, base);
            *im = round_to_type(ai, base);
            return 1;
        default:
            return 0;
    }
}

int sema_const_eval_complex(AST *expr, long double *re, long double *im) {
    return complex_eval(expr, re, im);
}

/* an integer constant expression with value 0, possibly cast to void * */
static int is_null_pointer_constant(AST *e) {
    long v;
    while (e->kind == N_IMPLICIT_CAST || (e->kind == N_CAST && type_is_void_pointer(e->type))) {
        e = e->u.cast.expr;
    }
    return e->type && type_is_integer(e->type) && sema_const_eval(e, &v) && v == 0;
}

static int is_address_constant(AST *e);

/* an lvalue designating (part of) an object with static storage, at a constant offset */
static int is_static_object(AST *o) {
    long v;
    switch (o->kind) {
        case N_IDENT:
            return o->sym && is_static_storage(o->sym);
        case N_STRING:
            return 1;
        case N_MEMBER:
            if (o->u.member.arrow) {
                return is_address_constant(o->u.member.object);
            }
            return is_static_object(o->u.member.object);
        case N_INDEX:
            return sema_const_eval(o->u.index.index, &v) && is_address_constant(o->u.index.array);
        case N_UNARY:
            return o->u.unary.op == OP_DEREF && is_address_constant(o->u.unary.operand);
        default:
            return 0;
    }
}

/* an address constant: the address of a static object or function, plus an offset */
static int is_address_constant(AST *e) {
    long v;
    switch (e->kind) {
        case N_IMPLICIT_CAST:
        case N_CAST: {
            AST *operand = e->u.cast.expr;
            if (operand->type && (operand->type->kind == TY_ARRAY || operand->type->kind == TY_FUNC)) {
                /* array or function decay */
                return is_static_object(operand);
            }
            return is_address_constant(operand) || sema_const_eval(operand, &v);
        }
        case N_IDENT:
            return e->sym && e->sym->kind == SYM_FUNCTION;
        case N_UNARY:
            return e->u.unary.op == OP_ADDR && is_static_object(e->u.unary.operand);
        case N_BINARY:
            if (e->u.binary.op == OP_ADD || e->u.binary.op == OP_SUB) {
                return (is_address_constant(e->u.binary.left) && sema_const_eval(e->u.binary.right, &v))
                    || (e->u.binary.op == OP_ADD && is_address_constant(e->u.binary.right)
                        && sema_const_eval(e->u.binary.left, &v));
            }
            return 0;
        default:
            return 0;
    }
}

/* may `e` initialise an object with static storage duration? */
static int is_constant_initializer(AST *e) {
    long v;
    long double d;
    if (!e->type) {
        return 1;   /* already diagnosed */
    }
    if (type_is_integer(e->type)) {
        return sema_const_eval(e, &v);
    }
    if (type_is_floating(e->type)) {
        return float_eval(e, &d);
    }
    if (type_is_complex(e->type)) {
        long double im;
        return complex_eval(e, &d, &im);
    }
    if (e->type->kind == TY_POINTER) {
        return is_address_constant(e) || sema_const_eval(e, &v);
    }
    return 0;
}

/* specifiers */

/* look for aligned(n) and packed in an attribute's text */
static void scan_attribute(const char *text, int *packed, size_t *aligned) {
    const char *p;
    if (strstr(text, "packed")) {
        *packed = 1;
    }
    p = strstr(text, "aligned");
    if (p) {
        p += strlen("aligned");
        while (*p == '_') {
            p++;
        }
        if (*p == '(') {
            long n = strtol(p + 1, NULL, 0);
            if (n > 0 && (size_t)n > *aligned) {
                *aligned = (size_t)n;
            }
        } else if (*aligned < 16) {
            *aligned = 16;  /* bare aligned: the largest alignment on either target */
        }
    }
}

static void sema_enum_spec(AST *spec, Type *type) {
    Record *r = type->record;
    Type *underlying = NULL;    /* the fixed underlying type, if one was given */
    long next = 0;
    int negative = 0;
    size_t i;

    if (!spec->u.record.has_body) {
        return;
    }
    if (r->complete) {
        diag_error(spec->loc, "redefinition of 'enum %s'", r->tag);
        return;
    }
    if (spec->u.record.underlying) {
        Specs us;
        sema_specs(spec->u.record.underlying, &us, 0, spec->loc);
        if (us.type && type_is_integer(us.type) && us.type->kind != TY_ENUM) {
            underlying = type_unqualified(us.type);
        } else {
            diag_error(spec->loc, "the underlying type of an enum must be an integer type");
        }
    }
    for (i = 0; i < spec->u.record.members->len; i++) {
        AST *en = spec->u.record.members->items[i];
        Symbol *sym = en->sym;
        if (en->u.enumerator.value) {
            long v;
            sema_rvalue(&en->u.enumerator.value);
            if (!type_is_integer(en->u.enumerator.value->type)
                    || !sema_const_eval(en->u.enumerator.value, &v)) {
                diag_error(en->loc, "enumerator value for '%s' is not an integer constant", en->u.enumerator.name);
                v = next;
            }
            next = v;
        }
        if (next < 0) {
            negative = 1;
        }
        /* with a fixed underlying type the enumerators have that type (C23) */
        en->type = underlying ? underlying : int_type();
        if (sym) {
            if (sym->type && sym->decl != en) {
                diag_error(en->loc, "redefinition of enumerator '%s'", en->u.enumerator.name);
                diag_note(sym->loc, "previous definition is here");
            } else {
                sym->type = en->type;
                sym->value = underlying ? next : (long)(int)next;
            }
        }
        next++;
    }
    if (underlying) {
        r->underlying = underlying;
        r->size = type_size(underlying);
        r->align = type_align(underlying);
        r->is_unsigned = type_is_unsigned(underlying);
    } else {
        r->size = arch_info->int_size;
        r->align = arch_info->int_size;
        /* gcc and clang give an enum without negative values the type unsigned int */
        r->is_unsigned = !negative;
    }
    r->complete = 1;
}

static void sema_struct_spec(AST *spec, Type *type, const Specs *attrs) {
    Record *r = type->record;
    size_t i;
    size_t j;

    if (!spec->u.record.has_body) {
        return;
    }
    if (r->complete) {
        diag_error(spec->loc, "redefinition of '%s %s'", type_kind_name(r->kind), r->tag);
        return;
    }
    for (i = 0; i < spec->u.record.members->len; i++) {
        AST *decl = spec->u.record.members->items[i];
        Specs sp;
        Specs *spp = &sp;
        Type *base;

        sema_specs(decl->u.decl.specs, spp, 0, decl->loc);
        base = sp.type;
        if (decl->u.decl.declarators->len == 0) {
            if (type_is_record(base) && !base->record->tag && type_is_complete(base)) {
                /* an anonymous struct or union: its members are reached as if they were this record's */
                record_add_member(r, NULL, base, -1, decl);
            } else if (!sp.only_tag) {
                diag_warning(decl->loc, "declaration does not declare anything");
            }
        }
        for (j = 0; j < decl->u.decl.declarators->len; j++) {
            AST *sd = decl->u.decl.declarators->items[j];
            AST *declarator = sd->u.init_decl.declarator;
            const char *name = NULL;
            Type *mt = base;
            int bits = -1;

            if (declarator) {
                mt = sema_declarator(declarator, base, &name);
            }
            if (sd->u.init_decl.init) {
                long w;
                sema_rvalue(&sd->u.init_decl.init);
                if (!type_is_integer(sd->u.init_decl.init->type)
                        || !sema_const_eval(sd->u.init_decl.init, &w)) {
                    diag_error(sd->loc, "bit-field width is not an integer constant");
                    w = 1;
                }
                if (!type_is_integer(mt)) {
                    diag_error(sd->loc, "bit-field '%s' has non-integer type %s", name ? name : "", ts(mt));
                    mt = int_type();
                }
                if (w < 0) {
                    diag_error(sd->loc, "bit-field has negative width");
                    w = 1;
                } else if ((size_t)w > type_size(mt) * 8) {
                    diag_error(sd->loc, "width of bit-field '%s' (%ld bits) exceeds the width of its type", name ? name : "", w);
                    w = (long)type_size(mt) * 8;
                } else if (w == 0 && name) {
                    diag_error(sd->loc, "named bit-field '%s' has zero width", name);
                }
                bits = (int)w;
            }
            if (bits < 0) {
                if (mt->kind == TY_FUNC) {
                    diag_error(sd->loc, "field '%s' declared as a function", name ? name : "");
                    mt = type_pointer(mt);
                } else if (!type_is_complete(mt)) {
                    /*
                     * A flexible array member (C99; the Linux headers use one):
                     * an array of unknown size as the last member of a struct
                     * that has another member. It takes no space in the layout.
                     */
                    int is_last = i + 1 == spec->u.record.members->len
                        && j + 1 == decl->u.decl.declarators->len;
                    if (!(mt->kind == TY_ARRAY && mt->length < 0 && type_is_complete(mt->base)
                          && r->kind == TY_STRUCT && is_last && r->nmembers > 0)) {
                        diag_error(sd->loc, "field '%s' has incomplete type %s", name ? name : "", ts(mt));
                        mt = int_type();
                    }
                }
            }
            if (name && record_find(r, name)) {
                diag_error(sd->loc, "duplicate member '%s'", name);
                continue;
            }
            sd->type = mt;
            if (sd->sym) {
                sd->sym->type = mt;
            }
            record_add_member(r, name, mt, bits, sd);
        }
    }
    r->packed = attrs->packed;
    r->aligned = attrs->aligned;
    r->pack = (size_t)spec->u.record.pack;
    if (spec->u.record.attrs) {
        for (i = 0; i < spec->u.record.attrs->len; i++) {
            scan_attribute(spec->u.record.attrs->items[i]->u.text, &r->packed, &r->aligned);
        }
    }
    record_layout(r);
}

/* the type a StructSpec or EnumSpec node denotes; creates the record on first sight */
static Type *tag_type(AST *spec) {
    Symbol *sym = spec->sym;
    TypeKind kind;
    Type *t;

    if (spec->kind == N_ENUM_SPEC) {
        kind = TY_ENUM;
    } else {
        kind = spec->u.record.is_union ? TY_UNION : TY_STRUCT;
    }
    if (sym && sym->type) {
        t = sym->type;
    } else {
        Record *r = record_new(kind, spec->u.record.tag);
        r->sym = sym;
        t = type_record(r);
        if (sym) {
            sym->type = t;
        }
    }
    spec->type = t;
    return t;
}

/*
 * Combine a specifier list into a type. `allow_storage` is 0 inside a
 * struct body, a parameter list or a type name.
 */
static void sema_specs(ASTList *specs, Specs *out, int allow_storage, Loc loc) {
    int n_void = 0, n_char = 0, n_short = 0, n_int = 0, n_long = 0;
    int n_float = 0, n_double = 0, n_signed = 0, n_unsigned = 0;
    int n_va_list = 0, n_int128 = 0, n_uint128 = 0, n_bool = 0, n_float16 = 0, n_complex = 0;
    int n_other = 0;        /* struct, union, enum, typedef name */
    AST *other = NULL;
    unsigned quals = 0;
    size_t i;
    Type *t = NULL;

    memset(out, 0, sizeof(*out));
    out->storage = STORAGE_NONE;

    for (i = 0; i < specs->len; i++) {
        AST *s = specs->items[i];
        switch (s->kind) {
            case N_STORAGE_CLASS:
                if (s->u.storage == STORAGE_INLINE) {
                    out->is_inline = 1;
                    break;
                }
                if (!allow_storage) {
                    diag_error(s->loc, "storage class specifier is not allowed here");
                    break;
                }
                if (out->storage != STORAGE_NONE) {
                    diag_error(s->loc, "multiple storage class specifiers");
                    break;
                }
                out->storage = s->u.storage;
                if (s->u.storage == STORAGE_TYPEDEF) {
                    out->is_typedef = 1;
                }
                break;

            case N_TYPE_QUAL:
                if (s->u.qual == QUAL_CONST) {
                    quals |= TQ_CONST;
                } else if (s->u.qual == QUAL_VOLATILE) {
                    quals |= TQ_VOLATILE;
                }
                break;

            case N_ATTRIBUTE:
                scan_attribute(s->u.text, &out->packed, &out->aligned);
                break;

            case N_TYPE_SPEC:
                switch (s->u.type_spec) {
                    case TS_VOID: n_void++; break;
                    case TS_CHAR: n_char++; break;
                    case TS_SHORT: n_short++; break;
                    case TS_INT: n_int++; break;
                    case TS_LONG: n_long++; break;
                    case TS_FLOAT: n_float++; break;
                    case TS_DOUBLE: n_double++; break;
                    case TS_SIGNED: n_signed++; break;
                    case TS_UNSIGNED: n_unsigned++; break;
                    case TS_BUILTIN_VA_LIST: n_va_list++; break;
                    case TS_INT128: n_int128++; break;
                    case TS_UINT128: n_uint128++; break;
                    case TS_BOOL: n_bool++; break;
                    case TS_FLOAT16: n_float16++; break;
                    case TS_COMPLEX: n_complex++; break;
                }
                break;

            case N_STRUCT_SPEC:
            case N_ENUM_SPEC:
            case N_TYPEDEF_NAME:
                n_other++;
                other = s;
                break;

            default:
                break;
        }
    }

    if (n_other > 1 || (n_other && (n_void + n_char + n_short + n_int + n_long + n_float
            + n_double + n_signed + n_unsigned + n_va_list + n_int128 + n_uint128 + n_bool + n_float16 + n_complex))) {
        diag_error(loc, "invalid combination of type specifiers");
        n_other = 0;
    }
    if (n_other) {
        if (other->kind == N_TYPEDEF_NAME) {
            Symbol *sym = other->sym;
            if (!sym || sym->kind != SYM_TYPEDEF || !sym->type) {
                diag_error(other->loc, "unknown type name '%s'", other->u.name);
                t = int_type();
            } else {
                t = sym->type;
            }
            out->only_tag = 0;
        } else {
            t = tag_type(other);
            if (other->kind == N_ENUM_SPEC) {
                sema_enum_spec(other, t);
            } else {
                sema_struct_spec(other, t, out);
            }
            out->only_tag = specs->len == 1 || (other->u.record.has_body);
        }
    } else if (n_void) {
        t = type_basic(TY_VOID);
        if (n_void > 1 || n_char || n_short || n_int || n_long || n_float || n_double || n_signed || n_unsigned) {
            diag_error(loc, "invalid combination of type specifiers");
        }
    } else if (n_va_list) {
        t = type_basic(TY_VA_LIST);
    } else if (n_float16) {
        t = type_basic(TY_FLOAT16);   /* declared by the macOS SDK's math.h; values are not supported */
    } else if (n_bool) {
        t = type_basic(TY_BOOL);
        if (n_bool > 1 || n_char || n_short || n_int || n_long || n_float || n_double || n_signed || n_unsigned) {
            diag_error(loc, "invalid combination of type specifiers");
        }
    } else if (n_int128 || n_uint128) {
        t = type_basic(n_uint128 || n_unsigned ? TY_UINT128 : TY_INT128);
    } else if (n_float) {
        t = type_basic(TY_FLOAT);
        if (n_float > 1 || n_char || n_short || n_int || n_long || n_double || n_signed || n_unsigned) {
            diag_error(loc, "invalid combination of type specifiers");
        }
    } else if (n_double) {
        t = type_basic(n_long ? TY_LDOUBLE : TY_DOUBLE);
        if (n_double > 1 || n_char || n_short || n_int || n_long > 1 || n_signed || n_unsigned) {
            diag_error(loc, "invalid combination of type specifiers");
        }
    } else {
        if (n_signed && n_unsigned) {
            diag_error(loc, "both 'signed' and 'unsigned' in declaration specifiers");
        }
        if (n_char) {
            t = type_basic(n_unsigned ? TY_UCHAR : n_signed ? TY_SCHAR : TY_CHAR);
            if (n_char > 1 || n_short || n_long || n_int) {
                diag_error(loc, "invalid combination of type specifiers");
            }
        } else if (n_short) {
            t = type_basic(n_unsigned ? TY_USHORT : TY_SHORT);
            if (n_short > 1 || n_long || n_int > 1) {
                diag_error(loc, "invalid combination of type specifiers");
            }
        } else if (n_long >= 2) {
            t = type_basic(n_unsigned ? TY_ULLONG : TY_LLONG);
            if (n_long > 2 || n_int > 1) {
                diag_error(loc, "invalid combination of type specifiers");
            }
        } else if (n_long == 1) {
            t = type_basic(n_unsigned ? TY_ULONG : TY_LONG);
            if (n_int > 1) {
                diag_error(loc, "invalid combination of type specifiers");
            }
        } else {
            /* int, signed, unsigned, or nothing at all (implicit int) */
            t = type_basic(n_unsigned ? TY_UINT : TY_INT);
            if (n_int > 1 || n_signed > 1 || n_unsigned > 1) {
                diag_error(loc, "invalid combination of type specifiers");
            }
        }
    }
    if (n_complex) {
        /* `_Complex` on a floating type, or alone for `double _Complex`; declared by <complex.h>, values are not supported */
        if (t->kind == TY_FLOAT) {
            t = type_basic(TY_CFLOAT);
        } else if (t->kind == TY_DOUBLE) {
            t = type_basic(TY_CDOUBLE);
        } else if (t->kind == TY_LDOUBLE) {
            t = type_basic(TY_CLDOUBLE);
        } else if (t->kind == TY_INT && !n_int && !n_signed && !n_unsigned) {
            t = type_basic(TY_CDOUBLE);
        } else {
            diag_error(loc, "'_Complex' requires a floating type, not %s", ts(t));
        }
    }
    out->type = type_qualify(t, quals);
}

/* qualifiers from a pointer's qualifier list */
static unsigned quals_of(ASTList *list) {
    unsigned q = 0;
    size_t i;
    if (!list) {
        return 0;
    }
    for (i = 0; i < list->len; i++) {
        AST *s = list->items[i];
        if (s->kind == N_TYPE_QUAL) {
            if (s->u.qual == QUAL_CONST) {
                q |= TQ_CONST;
            } else if (s->u.qual == QUAL_VOLATILE) {
                q |= TQ_VOLATILE;
            }
        }
    }
    return q;
}

/* a parameter's type after the array and function adjustments */
static Type *adjust_param(Type *t) {
    if (t->kind == TY_ARRAY) {
        return type_qualify(type_pointer(t->base), t->quals);
    }
    if (t->kind == TY_FUNC) {
        return type_pointer(t);
    }
    if (t->kind == TY_VA_LIST && arch_info->va_list_is_array) {
        return type_pointer(type_unqualified(t));
    }
    return t;
}

/*
 * Build the type a declarator gives to `base`. The chain runs from the
 * outermost constructor to the name, so each node wraps the type built so
 * far. `*name_out` gets the declared name, or NULL for an abstract one.
 */
/* the function declarator of the definition being analysed: the one place a K&R identifier list may appear */
static AST *defining_fn;

static Type *sema_declarator(AST *d, Type *base, const char **name_out) {
    Type *t = base;
    AST *node = d;

    *name_out = NULL;
    while (node) {
        switch (node->kind) {
            case N_DECL_NAME:
                *name_out = node->u.declarator.name;
                node = NULL;
                break;

            case N_DECL_POINTER:
                t = type_qualify(type_pointer(t), quals_of(node->u.declarator.quals));
                node = node->u.declarator.inner;
                break;

            case N_DECL_ARRAY: {
                long length = -1;
                if (t->kind == TY_FUNC) {
                    diag_error(node->loc, "array of functions");
                    t = type_pointer(t);
                } else if (!type_is_complete(t)) {
                    diag_error(node->loc, "array has incomplete element type %s", ts(t));
                    t = int_type();
                }
                if (node->u.declarator.size) {
                    long n;
                    sema_rvalue(&node->u.declarator.size);
                    if (!type_is_integer(node->u.declarator.size->type)
                            || !sema_const_eval(node->u.declarator.size, &n)) {
                        diag_error(node->loc, "array size is not an integer constant expression");
                        n = 1;
                    } else if (n < 0) {
                        /* a zero length is a GNU extension, common in system headers */
                        diag_error(node->loc, "array size is negative");
                        n = 1;
                    }
                    length = (long)n;
                }
                t = type_array(t, length);
                node = node->u.declarator.inner;
                break;
            }

            case N_DECL_FUNC: {
                ASTList *plist = node->u.declarator.params;
                Param *params = NULL;
                size_t nparams = 0;
                /* `()` says nothing about the arguments; `(void)` and a type list do */
                int has_proto = !node->u.declarator.kr && plist->len > 0;
                size_t i;

                if (node->u.declarator.kr && plist->len > 0 && node != defining_fn) {
                    diag_error(node->loc, "a parameter list without types is only allowed in a function definition");
                }

                if (t->kind == TY_FUNC) {
                    diag_error(node->loc, "function cannot return a function");
                    t = type_pointer(t);
                } else if (t->kind == TY_ARRAY) {
                    diag_error(node->loc, "function cannot return an array");
                    t = type_pointer(t->base);
                }
                if (has_proto && plist->len) {
                    params = xcalloc(plist->len, sizeof(*params));
                    for (i = 0; i < plist->len; i++) {
                        AST *p = plist->items[i];
                        Specs sp;
                        const char *pname = NULL;
                        Type *pt;

                        sema_specs(p->u.typed.specs, &sp, 1, p->loc);
                        if (sp.storage != STORAGE_NONE && sp.storage != STORAGE_REGISTER) {
                            diag_error(p->loc, "invalid storage class specifier in parameter declaration");
                        }
                        pt = sp.type;
                        if (p->u.typed.declarator) {
                            pt = sema_declarator(p->u.typed.declarator, sp.type, &pname);
                        }
                        if (pt->kind == TY_VOID && plist->len == 1 && !pname && !pt->quals) {
                            /* f(void) */
                            break;
                        }
                        if (pt->kind == TY_VOID) {
                            diag_error(p->loc, "parameter '%s' has incomplete type void", pname ? pname : "");
                            pt = int_type();
                        }
                        pt = adjust_param(pt);
                        p->type = pt;
                        if (p->sym) {
                            p->sym->type = pt;
                            p->sym->storage = sp.storage;
                        }
                        params[nparams].name = pname ? xstrdup(pname) : NULL;
                        params[nparams].type = pt;
                        nparams++;
                    }
                }
                t = type_func(t, params, nparams, node->u.declarator.variadic, has_proto);
                node = node->u.declarator.inner;
                break;
            }

            default:
                diag_error(node->loc, "internal error: bad declarator node");
                node = NULL;
                break;
        }
    }
    return t;
}

static Type *sema_type_name(AST *tn) {
    Specs sp;
    const char *name = NULL;
    Type *t;

    sema_specs(tn->u.typed.specs, &sp, 0, tn->loc);
    t = sp.type;
    if (tn->u.typed.declarator) {
        t = sema_declarator(tn->u.typed.declarator, sp.type, &name);
    }
    tn->type = t;
    return t;
}

/* expressions */

/* insert a conversion of *slot to `to` unless it already has that type */
static void coerce(AST **slot, Type *to) {
    Type *from = (*slot)->type;
    if (from == to || (type_compatible(type_unqualified(from), type_unqualified(to))
            && from->kind != TY_POINTER && from->kind != TY_ARRAY)) {
        return;
    }
    if (from->kind == TY_POINTER && to->kind == TY_POINTER && type_compatible(from, to)) {
        return;
    }
    ast_implicit_cast(slot, to);
}

/* qualifiers of what a pointer points to */
static unsigned pointee_quals(const Type *p) {
    return p->base->quals;
}

/*
 * The constraints on assignment, also used for argument passing,
 * initialisation and return. `what` names the context for messages.
 * The operand has been converted to an rvalue.
 */
static void convert_assign(AST **slot, Type *to, const char *what) {
    AST *e = *slot;
    Type *from = e->type;
    Loc loc = e->loc;

    to = type_unqualified(to);
    if (type_is_arithmetic(to) && type_is_arithmetic(from)) {
        coerce(slot, to);
        return;
    }
    if (to->kind == TY_BOOL && from->kind == TY_POINTER) {
        /* a pointer converts to _Bool: null is 0 */
        coerce(slot, to);
        return;
    }
    if (type_is_record(to)) {
        if (!type_compatible_unqual(to, from)) {
            diag_error(loc, "%s %s from incompatible type %s", what, ts(to), ts(from));
        }
        return;
    }
    if (to->kind == TY_VA_LIST) {
        if (from->kind != TY_VA_LIST) {
            diag_error(loc, "%s va_list from incompatible type %s", what, ts(from));
        }
        return;
    }
    if (to->kind == TY_POINTER) {
        if (from->kind == TY_POINTER) {
            if (type_is_void_pointer(to) || type_is_void_pointer(from)
                    || type_compatible_unqual(to->base, from->base)) {
                if ((pointee_quals(from) & ~pointee_quals(to)) != 0) {
                    diag_warning(loc, "%s %s from %s discards qualifiers", what, ts(to), ts(from));
                }
            } else {
                diag_warning(loc, "%s %s from incompatible pointer type %s", what, ts(to), ts(from));
            }
            coerce(slot, to);
            return;
        }
        if (type_is_integer(from)) {
            if (!is_null_pointer_constant(e)) {
                diag_warning(loc, "%s %s from integer type %s", what, ts(to), ts(from));
            }
            coerce(slot, to);
            return;
        }
        diag_error(loc, "%s %s from incompatible type %s", what, ts(to), ts(from));
        return;
    }
    if (type_is_integer(to) && from->kind == TY_POINTER) {
        diag_warning(loc, "%s %s from pointer type %s", what, ts(to), ts(from));
        coerce(slot, to);
        return;
    }
    if (to->kind == TY_VOID) {
        return;
    }
    diag_error(loc, "%s %s from incompatible type %s", what, ts(to), ts(from));
}

/*
 * A bit-field promotes by its width (3.2.1.1): to int when an int holds
 * every value it can take, else to unsigned int; a field wider than an int
 * (an extension) keeps its declared type. The member keeps its declared
 * type for assignment; this is for operands.
 */
static Type *promote_bitfield(AST **slot) {
    AST *e = *slot;
    size_t int_bits;
    size_t bits;
    Type *p;

    if (!sema_is_bitfield(e) || !type_is_integer(e->type)) {
        return e->type;
    }
    int_bits = type_size(int_type()) * 8;
    bits = (size_t)e->u.member.member->bits;
    if (bits < int_bits || (bits == int_bits && !type_is_unsigned(e->type))) {
        p = int_type();
    } else if (bits == int_bits) {
        p = type_basic(TY_UINT);
    } else {
        p = type_promote(e->type);
    }
    coerce(slot, p);
    return p;
}

/* the default argument promotions, for unprototyped and variadic calls */
static void default_promote(AST **slot) {
    Type *t = promote_bitfield(slot);
    if (t->kind == TY_FLOAT) {
        coerce(slot, type_basic(TY_DOUBLE));
    } else if (type_is_integer(t)) {
        coerce(slot, type_promote(t));
    }
}

/* a struct or union (or array of them) with a const-qualified member, however deep: not a modifiable lvalue */
static int has_const_member(const Type *t) {
    size_t i;
    if (t->kind == TY_ARRAY) {
        return has_const_member(t->base);
    }
    if ((t->kind != TY_STRUCT && t->kind != TY_UNION) || !t->record->complete) {
        return 0;
    }
    for (i = 0; i < t->record->nmembers; i++) {
        const Type *mt = t->record->members[i].type;
        if ((mt->quals & TQ_CONST) || has_const_member(mt)) {
            return 1;
        }
    }
    return 0;
}

/* the object an lvalue designates part of: through `.` members and subscripts of arrays, not through pointers */
static const Symbol *designated_object(const AST *e) {
    for (;;) {
        if (e->kind == N_MEMBER && !e->u.member.arrow) {
            e = e->u.member.object;
        } else if (e->kind == N_INDEX && e->u.index.array->type && e->u.index.array->type->kind == TY_ARRAY) {
            e = e->u.index.array;
        } else {
            return e->kind == N_IDENT ? e->sym : NULL;
        }
    }
}

static int check_modifiable(AST *e, const char *what) {
    if (poisoned(e)) {
        return 0;
    }
    if (!e->lvalue) {
        diag_error(e->loc, "%s requires a modifiable lvalue", what);
        return 0;
    }
    if (e->type->kind == TY_ARRAY) {
        diag_error(e->loc, "%s an array is not allowed", what);
        return 0;
    }
    if (!type_is_complete(e->type)) {
        diag_error(e->loc, "%s an object of incomplete type %s", what, ts(e->type));
        return 0;
    }
    if (e->type->quals & TQ_CONST) {
        diag_error(e->loc, "%s a const-qualified object of type %s", what, ts(e->type));
        return 0;
    }
    if (has_const_member(e->type)) {
        diag_error(e->loc, "%s %s is not allowed: it has a const-qualified member", what, ts(e->type));
        return 0;
    }
    return 1;
}

/* integer promotion of a single operand, in place */
static Type *promote_operand(AST **slot) {
    Type *p = type_promote(promote_bitfield(slot));
    coerce(slot, p);
    return p;
}

static Type *sema_ident(AST *e) {
    Symbol *sym = e->sym;

    if (!sym) {
        Type *bt = builtin_type(e->u.name);
        if (bt) {
            sym = symtab_declare(cur_symtab, NS_ORDINARY, SYM_FUNCTION, e->u.name, NULL, e->loc);
            sym->type = bt;
            e->sym = sym;
        } else {
            diag_error(e->loc, "use of undeclared identifier '%s'", e->u.name);
            return error_type(e);
        }
    }
    switch (sym->kind) {
        case SYM_VARIABLE:
        case SYM_PARAM:
            if (!sym->type) {
                /* declaration failed earlier, or a K&R parameter used in its own declaration */
                return error_type(e);
            }
            e->type = sym->type;
            e->lvalue = 1;
            return e->type;

        case SYM_FUNCTION:
            if (!sym->type) {
                return error_type(e);
            }
            e->type = sym->type;
            e->lvalue = 0;
            return e->type;

        case SYM_ENUMERATOR:
            /* an enumerator of an enum with a fixed underlying type has that type */
            e->type = sym->type ? sym->type : int_type();
            e->lvalue = 0;
            return e->type;

        case SYM_TYPEDEF:
            diag_error(e->loc, "unexpected type name '%s'", e->u.name);
            return error_type(e);

        default:
            diag_error(e->loc, "'%s' is not a value", e->u.name);
            return error_type(e);
    }
}

static Type *sema_unary(AST *e) {
    AST **operand = &e->u.unary.operand;
    Type *t;

    switch (e->u.unary.op) {
        case OP_ADDR:
            t = sema_expr(operand);
            if (sema_is_bitfield(*operand)) {
                diag_error(e->loc, "address of bit-field requested");
                return error_type(e);
            }
            if (poisoned(*operand)) {
                return error_type(e);
            }
            if (t->kind != TY_FUNC && !(*operand)->lvalue) {
                diag_error(e->loc, "cannot take the address of an rvalue of type %s", ts(t));
                return error_type(e);
            }
            {
                const Symbol *obj = designated_object(*operand);
                if (obj && obj->storage == STORAGE_REGISTER) {
                    diag_error(e->loc, "address of register variable '%s' requested", obj->name);
                    return error_type(e);
                }
            }
            e->type = type_pointer(t);
            e->lvalue = 0;
            return e->type;

        case OP_DEREF:
            t = sema_rvalue(operand);
            if (t->kind != TY_POINTER) {
                diag_error(e->loc, "indirection requires a pointer operand, got %s", ts(t));
                return error_type(e);
            }
            if (t->base->kind == TY_VOID) {
                diag_error(e->loc, "indirection through a pointer to void");
                return error_type(e);
            }
            e->type = t->base;
            e->lvalue = t->base->kind != TY_FUNC;
            return e->type;

        case OP_POS:
        case OP_NEG:
            t = sema_rvalue(operand);
            if (!type_is_arithmetic(t)) {
                diag_error(e->loc, "invalid operand of type %s to unary '%s'", ts(t), op_name(e->u.unary.op));
                return error_type(e);
            }
            e->type = type_is_integer(t) ? promote_operand(operand) : t;
            e->lvalue = 0;
            return e->type;

        case OP_BITNOT:
            t = sema_rvalue(operand);
            if (type_is_complex(t)) {
                e->type = t;    /* GNU: the complex conjugate */
                e->lvalue = 0;
                return e->type;
            }
            if (!type_is_integer(t)) {
                diag_error(e->loc, "invalid operand of type %s to unary '~'", ts(t));
                return error_type(e);
            }
            e->type = promote_operand(operand);
            e->lvalue = 0;
            return e->type;

        case OP_NOT:
            t = sema_rvalue(operand);
            if (!type_is_scalar(t)) {
                diag_error(e->loc, "invalid operand of type %s to unary '!'", ts(t));
                return error_type(e);
            }
            e->type = int_type();
            e->lvalue = 0;
            return e->type;

        case OP_REAL:
        case OP_IMAG:
            /* GNU: the components of a complex lvalue are lvalues; on a real operand __real__ is the operand and __imag__ is 0 */
            t = sema_expr(operand);
            if (type_is_complex(t)) {
                e->type = type_qualify(type_complex_base(t), t->quals);
                e->lvalue = (*operand)->lvalue;
                return e->type;
            }
            if (!type_is_arithmetic(t)) {
                diag_error(e->loc, "invalid operand of type %s to '%s'", ts(t), op_name(e->u.unary.op));
                return error_type(e);
            }
            e->type = e->u.unary.op == OP_REAL ? t : type_unqualified(t);
            e->lvalue = e->u.unary.op == OP_REAL && (*operand)->lvalue;
            return e->type;

        case OP_PREINC:
        case OP_PREDEC:
        case OP_POSTINC:
        case OP_POSTDEC:
            t = sema_expr(operand);
            if (!check_modifiable(*operand, "incrementing or decrementing")) {
                return error_type(e);
            }
            if (!type_is_scalar(t)) {
                diag_error(e->loc, "cannot increment or decrement a value of type %s", ts(t));
                return error_type(e);
            }
            if (t->kind == TY_POINTER && !type_is_complete(t->base) && t->base->kind != TY_VOID) {
                diag_error(e->loc, "arithmetic on a pointer to incomplete type %s", ts(t));
                return error_type(e);
            }
            e->type = type_unqualified(t);
            e->lvalue = 0;
            return e->type;

        default:
            diag_error(e->loc, "internal error: bad unary operator");
            return error_type(e);
    }
}

static int pointer_arith_ok(AST *e, Type *p) {
    if (p->base->kind == TY_FUNC) {
        diag_error(e->loc, "arithmetic on a pointer to a function");
        return 0;
    }
    if (!type_is_complete(p->base) && p->base->kind != TY_VOID) {
        diag_error(e->loc, "arithmetic on a pointer to incomplete type %s", ts(p));
        return 0;
    }
    return 1;
}

static Type *sema_binary(AST *e) {
    AST **l = &e->u.binary.left;
    AST **r = &e->u.binary.right;
    OpKind op = e->u.binary.op;
    Type *lt;
    Type *rt;
    Type *common;

    e->lvalue = 0;
    if (op == OP_COMMA) {
        sema_rvalue(l);
        e->type = sema_rvalue(r);
        return e->type;
    }
    lt = sema_rvalue(l);
    rt = sema_rvalue(r);
    if (sema_is_bitfield(*l)) {
        lt = promote_bitfield(l);
    }
    if (sema_is_bitfield(*r)) {
        rt = promote_bitfield(r);
    }

    switch (op) {
        case OP_MUL:
        case OP_DIV:
            if (!type_is_arithmetic(lt) || !type_is_arithmetic(rt)) {
                break;
            }
            common = type_arith_common(lt, rt);
            coerce(l, common);
            coerce(r, common);
            e->type = common;
            return common;

        case OP_MOD:
        case OP_BITAND:
        case OP_BITXOR:
        case OP_BITOR:
            if (!type_is_integer(lt) || !type_is_integer(rt)) {
                break;
            }
            common = type_arith_common(lt, rt);
            coerce(l, common);
            coerce(r, common);
            e->type = common;
            return common;

        case OP_SHL:
        case OP_SHR:
            if (!type_is_integer(lt) || !type_is_integer(rt)) {
                break;
            }
            e->type = promote_operand(l);
            promote_operand(r);
            return e->type;

        case OP_ADD:
            if (type_is_arithmetic(lt) && type_is_arithmetic(rt)) {
                common = type_arith_common(lt, rt);
                coerce(l, common);
                coerce(r, common);
                e->type = common;
                return common;
            }
            if (lt->kind == TY_POINTER && type_is_integer(rt)) {
                if (!pointer_arith_ok(e, lt)) {
                    return error_type(e);
                }
                e->type = lt;
                return lt;
            }
            if (type_is_integer(lt) && rt->kind == TY_POINTER) {
                if (!pointer_arith_ok(e, rt)) {
                    return error_type(e);
                }
                e->type = rt;
                return rt;
            }
            break;

        case OP_SUB:
            if (type_is_arithmetic(lt) && type_is_arithmetic(rt)) {
                common = type_arith_common(lt, rt);
                coerce(l, common);
                coerce(r, common);
                e->type = common;
                return common;
            }
            if (lt->kind == TY_POINTER && type_is_integer(rt)) {
                if (!pointer_arith_ok(e, lt)) {
                    return error_type(e);
                }
                e->type = lt;
                return lt;
            }
            if (lt->kind == TY_POINTER && rt->kind == TY_POINTER) {
                if (!type_compatible_unqual(lt->base, rt->base)) {
                    diag_error(e->loc, "subtracting pointers to incompatible types %s and %s", ts(lt), ts(rt));
                    return error_type(e);
                }
                if (!pointer_arith_ok(e, lt)) {
                    return error_type(e);
                }
                e->type = type_ptrdiff_t();
                return e->type;
            }
            break;

        case OP_LT:
        case OP_GT:
        case OP_LE:
        case OP_GE:
        case OP_EQ:
        case OP_NE:
            e->type = int_type();
            if (op != OP_EQ && op != OP_NE && (type_is_complex(lt) || type_is_complex(rt))) {
                diag_error(e->loc, "ordering comparison of complex values (%s and %s)", ts(lt), ts(rt));
                return error_type(e);
            }
            if (type_is_arithmetic(lt) && type_is_arithmetic(rt)) {
                common = type_arith_common(lt, rt);
                coerce(l, common);
                coerce(r, common);
                return e->type;
            }
            if (lt->kind == TY_POINTER && rt->kind == TY_POINTER) {
                int equality = op == OP_EQ || op == OP_NE;
                if (!type_compatible_unqual(lt->base, rt->base)
                        && !(equality && (type_is_void_pointer(lt) || type_is_void_pointer(rt)))) {
                    diag_warning(e->loc, "comparison of distinct pointer types %s and %s", ts(lt), ts(rt));
                }
                coerce(r, lt);
                return e->type;
            }
            if (lt->kind == TY_POINTER && type_is_integer(rt)) {
                if (!is_null_pointer_constant(*r)) {
                    diag_warning(e->loc, "comparison between pointer %s and integer %s", ts(lt), ts(rt));
                }
                coerce(r, lt);
                return e->type;
            }
            if (type_is_integer(lt) && rt->kind == TY_POINTER) {
                if (!is_null_pointer_constant(*l)) {
                    diag_warning(e->loc, "comparison between integer %s and pointer %s", ts(lt), ts(rt));
                }
                coerce(l, rt);
                return e->type;
            }
            break;

        case OP_AND:
        case OP_OR:
            if (!type_is_scalar(lt) || !type_is_scalar(rt)) {
                break;
            }
            e->type = int_type();
            return e->type;

        default:
            break;
    }
    diag_error(e->loc, "invalid operands to binary '%s' (%s and %s)", op_name(op), ts(lt), ts(rt));
    return error_type(e);
}

static Type *sema_assign(AST *e) {
    AST **l = &e->u.binary.left;
    AST **r = &e->u.binary.right;
    OpKind op = e->u.binary.op;
    Type *lt;
    Type *rt;
    Type *result;

    lt = sema_expr(l);
    rt = sema_rvalue(r);
    e->lvalue = 0;
    if (!check_modifiable(*l, "assigning to")) {
        return error_type(e);
    }
    result = type_unqualified(lt);
    e->type = result;

    if (op == OP_ASSIGN) {
        convert_assign(r, result, "assigning");
        return result;
    }
    if ((op == OP_ADD_ASSIGN || op == OP_SUB_ASSIGN) && lt->kind == TY_POINTER && type_is_integer(rt)) {
        if (!pointer_arith_ok(e, lt)) {
            return error_type(e);
        }
        return result;
    }
    if (op == OP_ADD_ASSIGN || op == OP_SUB_ASSIGN || op == OP_MUL_ASSIGN || op == OP_DIV_ASSIGN) {
        if (!type_is_arithmetic(lt) || !type_is_arithmetic(rt)) {
            diag_error(e->loc, "invalid operands to '%s' (%s and %s)", op_name(op), ts(lt), ts(rt));
            return error_type(e);
        }
    } else {
        if (!type_is_integer(lt) || !type_is_integer(rt)) {
            diag_error(e->loc, "invalid operands to '%s' (%s and %s)", op_name(op), ts(lt), ts(rt));
            return error_type(e);
        }
    }
    /* the operation is carried out in the common type and the result converted back */
    if (op == OP_SHL_ASSIGN || op == OP_SHR_ASSIGN) {
        promote_operand(r);
    } else {
        coerce(r, type_arith_common(lt, rt));
    }
    return result;
}

static Type *sema_cond(AST *e) {
    Type *ct = sema_rvalue(&e->u.cond.cond);
    AST **a = &e->u.cond.then;
    AST **b = &e->u.cond.otherwise;
    Type *at;
    Type *bt;

    e->lvalue = 0;
    if (!type_is_scalar(ct)) {
        diag_error(e->loc, "condition of type %s is not scalar", ts(ct));
    }
    at = sema_rvalue(a);
    bt = sema_rvalue(b);
    if (type_is_arithmetic(at) && type_is_arithmetic(bt)) {
        Type *common = type_arith_common(at, bt);
        coerce(a, common);
        coerce(b, common);
        e->type = common;
        return common;
    }
    if (type_is_record(at) && type_is_record(bt) && type_compatible_unqual(at, bt)) {
        e->type = type_unqualified(at);
        return e->type;
    }
    if (at->kind == TY_VOID || bt->kind == TY_VOID) {
        /* both void in ISO C; gcc allows one, as with a statement expression ending in a jump */
        e->type = type_basic(TY_VOID);
        return e->type;
    }
    if (at->kind == TY_POINTER && bt->kind == TY_POINTER) {
        unsigned q = pointee_quals(at) | pointee_quals(bt);
        Type *result;
        if (type_is_void_pointer(at) && !type_is_void_pointer(bt)) {
            result = type_pointer(type_qualify(type_basic(TY_VOID), q));
        } else if (type_is_void_pointer(bt)) {
            result = type_pointer(type_qualify(type_basic(TY_VOID), q));
        } else if (type_compatible_unqual(at->base, bt->base)) {
            result = type_pointer(type_qualify(type_unqualified(at->base), q));
        } else {
            diag_warning(e->loc, "pointer type mismatch in conditional expression (%s and %s)", ts(at), ts(bt));
            result = type_pointer(type_basic(TY_VOID));
        }
        coerce(a, result);
        coerce(b, result);
        e->type = result;
        return result;
    }
    if (at->kind == TY_POINTER && type_is_integer(bt)) {
        if (!is_null_pointer_constant(*b)) {
            diag_warning(e->loc, "pointer and integer mismatch in conditional expression (%s and %s)", ts(at), ts(bt));
        }
        coerce(b, at);
        e->type = at;
        return at;
    }
    if (type_is_integer(at) && bt->kind == TY_POINTER) {
        if (!is_null_pointer_constant(*a)) {
            diag_warning(e->loc, "pointer and integer mismatch in conditional expression (%s and %s)", ts(at), ts(bt));
        }
        coerce(a, bt);
        e->type = bt;
        return bt;
    }
    diag_error(e->loc, "incompatible operand types in conditional expression (%s and %s)", ts(at), ts(bt));
    return error_type(e);
}

static Type *sema_call(AST *e) {
    AST **f = &e->u.call.func;
    ASTList *args = e->u.call.args;
    Type *ft;
    Type *fn;
    size_t i;

    e->lvalue = 0;
    if ((*f)->kind == N_IDENT && !(*f)->sym) {
        if (strcmp((*f)->u.name, "__atomic_load_n") == 0) {
            return sema_atomic(e, 0);
        }
        if (strcmp((*f)->u.name, "__atomic_store_n") == 0) {
            return sema_atomic(e, 1);
        }
    }
    if ((*f)->kind == N_IDENT && !(*f)->sym && !builtin_type((*f)->u.name)) {
        /* C89 implicit declaration: int f() */
        Symbol *sym;
        diag_warning((*f)->loc, "implicit declaration of function '%s'", (*f)->u.name);
        sym = symtab_declare(cur_symtab, NS_ORDINARY, SYM_FUNCTION, (*f)->u.name, NULL, (*f)->loc);
        if (!sym->type) {
            sym->type = type_func(int_type(), NULL, 0, 0, 0);
        }
        (*f)->sym = sym;
    }
    ft = sema_rvalue(f);
    if (!type_is_func_pointer(ft)) {
        diag_error(e->loc, "called object of type %s is not a function or function pointer", ts(ft));
        for (i = 0; i < args->len; i++) {
            sema_rvalue(&args->items[i]);
        }
        return error_type(e);
    }
    fn = ft->base;
    for (i = 0; i < args->len; i++) {
        Type *at = sema_rvalue(&args->items[i]);
        if (at->kind == TY_VOID) {
            diag_error(args->items[i]->loc, "argument has type void");
        }
    }
    if (fn->has_proto) {
        if (args->len < fn->nparams) {
            diag_error(e->loc, "too few arguments to function call, expected %lu, have %lu",
                       (unsigned long)fn->nparams, (unsigned long)args->len);
        } else if (args->len > fn->nparams && !fn->variadic) {
            diag_error(e->loc, "too many arguments to function call, expected %lu, have %lu",
                       (unsigned long)fn->nparams, (unsigned long)args->len);
        }
        for (i = 0; i < args->len; i++) {
            if (i < fn->nparams) {
                char what[64];
                snprintf(what, sizeof(what), "passing argument %lu of type", (unsigned long)i + 1);
                convert_assign(&args->items[i], fn->params[i].type, what);
            } else {
                default_promote(&args->items[i]);
            }
        }
    } else {
        for (i = 0; i < args->len; i++) {
            default_promote(&args->items[i]);
        }
    }
    if (!type_is_complete(fn->base) && fn->base->kind != TY_VOID) {
        diag_error(e->loc, "calling a function that returns incomplete type %s", ts(fn->base));
    }
    e->type = fn->base;
    return e->type;
}

static Type *sema_index(AST *e) {
    AST **a = &e->u.index.array;
    AST **i = &e->u.index.index;
    Type *at = sema_rvalue(a);
    Type *it = sema_rvalue(i);
    Type *p;

    if (at->kind == TY_POINTER && type_is_integer(it)) {
        p = at;
    } else if (it->kind == TY_POINTER && type_is_integer(at)) {
        p = it;
    } else if (at->kind != TY_POINTER && it->kind != TY_POINTER) {
        diag_error(e->loc, "subscripted value of type %s is not an array or pointer", ts(at));
        return error_type(e);
    } else {
        diag_error(e->loc, "array subscript of type %s is not an integer", ts(at->kind == TY_POINTER ? it : at));
        return error_type(e);
    }
    if (!pointer_arith_ok(e, p)) {
        return error_type(e);
    }
    e->type = p->base;
    e->lvalue = p->base->kind != TY_FUNC;
    return e->type;
}

static Type *sema_member(AST *e) {
    AST **o = &e->u.member.object;
    Type *ot;
    Type *rt;
    Member *m;

    if (e->u.member.arrow) {
        ot = sema_rvalue(o);
        if (ot->kind != TY_POINTER || !type_is_record(ot->base)) {
            diag_error(e->loc, "member reference '->%s' on type %s, which is not a pointer to a struct or union",
                       e->u.member.name, ts(ot));
            return error_type(e);
        }
        rt = ot->base;
        e->lvalue = 1;
    } else {
        ot = sema_expr(o);
        if (!type_is_record(ot)) {
            diag_error(e->loc, "member reference '.%s' on type %s, which is not a struct or union",
                       e->u.member.name, ts(ot));
            return error_type(e);
        }
        rt = ot;
        e->lvalue = (*o)->lvalue > 0;
    }
    if (!rt->record->complete) {
        diag_error(e->loc, "member access into incomplete type %s", ts(rt));
        return error_type(e);
    }
    {
        Member *path[RECORD_PATH_MAX];
        size_t depth;
        size_t k;
        m = record_lookup(rt->record, e->u.member.name, path, &depth);
        if (!m) {
            diag_error(e->loc, "no member named '%s' in %s", e->u.member.name, ts(rt));
            return error_type(e);
        }
        /* a member of an anonymous struct or union: reach it through the anonymous member, as if it were named */
        for (k = 0; k < depth; k++) {
            AST *w = ast_new(N_MEMBER, e->loc);
            w->u.member.object = *o;
            w->u.member.name = xstrdup("");
            w->u.member.arrow = e->u.member.arrow;
            w->u.member.member = path[k];
            w->type = type_qualify(path[k]->type, rt->quals);
            w->lvalue = e->lvalue;
            *o = w;
            e->u.member.arrow = 0;
        }
    }
    e->u.member.member = m;
    e->type = type_qualify(m->type, rt->quals);
    return e->type;
}

static Type *sema_cast(AST *e) {
    Type *to = sema_type_name(e->u.cast.type);
    Type *from = sema_rvalue(&e->u.cast.expr);

    e->lvalue = 0;
    e->type = type_unqualified(to);
    if (to->kind == TY_VOID) {
        return e->type;
    }
    if (!type_is_scalar(to)) {
        diag_error(e->loc, "cast to non-scalar type %s", ts(to));
        return error_type(e);
    }
    if (!type_is_scalar(from)) {
        diag_error(e->loc, "operand of type %s cannot be cast to %s", ts(from), ts(to));
        return error_type(e);
    }
    if ((to->kind == TY_POINTER && (type_is_floating(from) || type_is_complex(from)))
            || (from->kind == TY_POINTER && (type_is_floating(to) || type_is_complex(to)))) {
        diag_error(e->loc, "cast between pointer and floating type (%s to %s)", ts(from), ts(to));
        return error_type(e);
    }
    return e->type;
}

static Type *sema_sizeof(AST *e) {
    Type *t;

    e->lvalue = 0;
    e->type = type_size_t();
    if (e->kind == N_SIZEOF_TYPE) {
        t = sema_type_name(e->u.cast.type);
    } else {
        t = sema_expr(&e->u.cast.expr);
        if (sema_is_bitfield(e->u.cast.expr)) {
            diag_error(e->loc, "invalid application of 'sizeof' to a bit-field");
            return e->type;
        }
    }
    if (t->kind == TY_FUNC) {
        /* a GNU extension: the size is 1 */
        diag_warning(e->loc, "invalid application of 'sizeof' to a function type");
    } else if (!type_is_complete(t) && t->kind != TY_VOID) {
        diag_error(e->loc, "invalid application of 'sizeof' to incomplete type %s", ts(t));
    }
    return e->type;
}

/* __builtin_va_arg(ap, type): `ap` is a va_list object, or the pointer it decays to */
static Type *sema_va_arg(AST *e) {
    Type *at = sema_rvalue(&e->u.cast.expr);
    Type *t = sema_type_name(e->u.cast.type);

    e->lvalue = 0;
    if (at->kind != TY_VA_LIST && !(at->kind == TY_POINTER && at->base->kind == TY_VA_LIST)) {
        diag_error(e->u.cast.expr->loc, "first argument to va_arg has type %s, not va_list", ts(at));
        return error_type(e);
    }
    if (!type_is_complete(t)) {
        diag_error(e->loc, "va_arg of incomplete type %s", ts(t));
        return error_type(e);
    }
    e->type = type_unqualified(t);
    return e->type;
}

static Type *sema_expr(AST **slot) {
    AST *e = *slot;

    switch (e->kind) {
        case N_IDENT:
            return sema_ident(e);

        case N_INT: {
            unsigned long v;
            e->type = int_literal_type(e->u.text, &v);
            e->lvalue = 0;
            return e->type;
        }

        case N_FLOAT:
            e->type = float_literal_type(e->u.text);
            e->lvalue = 0;
            return e->type;

        case N_CHAR:
            e->type = int_type();
            e->lvalue = 0;
            return e->type;

        case N_STRING: {
            size_t n = decode_literal(e->u.text, NULL, NULL, 1);
            e->type = type_array(literal_is_wide(e->u.text) ? int_type() : type_basic(TY_CHAR), (long)n + 1);
            e->lvalue = 1;
            return e->type;
        }

        case N_UNARY:
            return sema_unary(e);

        case N_BINARY:
            return sema_binary(e);

        case N_ASSIGN:
            return sema_assign(e);

        case N_COND:
            return sema_cond(e);

        case N_CALL:
            return sema_call(e);

        case N_INDEX:
            return sema_index(e);

        case N_MEMBER:
            return sema_member(e);

        case N_CAST:
            return sema_cast(e);

        case N_SIZEOF_EXPR:
        case N_SIZEOF_TYPE:
            return sema_sizeof(e);

        case N_VA_ARG:
            return sema_va_arg(e);

        case N_IMPLICIT_CAST:
            /* already analysed */
            return e->type;

        case N_OFFSETOF: {
            Type *t = sema_type_name(e->u.desig.init);
            unsigned long off = 0;
            size_t k;
            char text[32];
            for (k = 0; k < e->u.desig.path->len; k++) {
                AST *d = e->u.desig.path->items[k];
                if (d->kind == N_DESIG_MEMBER) {
                    Member *m;
                    Member *path[RECORD_PATH_MAX];
                    size_t depth;
                    size_t j;
                    if (!type_is_record(t) || !type_is_complete(t)) {
                        diag_error(d->loc, "offsetof requires a complete struct or union type, not %s", ts(t));
                        return error_type(e);
                    }
                    m = record_lookup(t->record, d->u.name, path, &depth);
                    if (!m) {
                        diag_error(d->loc, "no member named '%s' in %s", d->u.name, ts(t));
                        return error_type(e);
                    }
                    for (j = 0; j < depth; j++) {
                        off += path[j]->offset;
                    }
                    off += m->offset;
                    t = m->type;
                } else {
                    long idx;
                    if (t->kind != TY_ARRAY) {
                        diag_error(d->loc, "subscript in offsetof of non-array type %s", ts(t));
                        return error_type(e);
                    }
                    sema_rvalue(&d->u.cast.expr);
                    if (!sema_const_eval(d->u.cast.expr, &idx)) {
                        diag_error(d->loc, "offsetof index is not a constant expression");
                        return error_type(e);
                    }
                    off += (unsigned long)idx * type_size(t->base);
                    t = t->base;
                }
            }
            snprintf(text, sizeof(text), "%luUL", off);
            *slot = ast_text(N_INT, e->loc, xstrdup(text));
            ast_free(e);
            return sema_expr(slot);
        }

        case N_STMT_EXPR: {
            ASTList *items = e->u.stmt.body->u.block.items;
            AST *last = items->len ? items->items[items->len - 1] : NULL;
            sema_stmt(e->u.stmt.body);
            e->lvalue = 0;
            if (last && last->kind == N_EXPR_STMT && last->u.stmt.expr) {
                e->type = type_unqualified(last->u.stmt.expr->type);
            } else {
                e->type = type_basic(TY_VOID);
            }
            return e->type;
        }

        case N_INIT_LIST:
            diag_error(e->loc, "initializer list used where an expression is required");
            return error_type(e);

        default:
            diag_error(e->loc, "internal error: %s is not an expression", node_kind_name(e->kind));
            return error_type(e);
    }
}

/*
 * The lvalue conversion: arrays become pointers to their first element,
 * functions become pointers to themselves, everything else loses its
 * qualifiers.
 */
static Type *sema_rvalue(AST **slot) {
    Type *t = sema_expr(slot);
    Type *d;

    if (t->kind == TY_ARRAY || t->kind == TY_FUNC || (t->kind == TY_VA_LIST && arch_info->va_list_is_array)) {
        d = type_decay(t);
        ast_implicit_cast(slot, d);
        return d;
    }
    return type_unqualified(t);
}

/* initializers */

/*
 * A cursor walks a brace-enclosed list. An item may carry C99 designators
 * (`.a[2].b = x`); `path_pos` is how many of them the walk has already
 * applied to position itself, and the item's own initializer is reached
 * once all of them are.
 */
typedef struct Cursor {
    ASTList *list;
    size_t pos;
    size_t path_pos;
} Cursor;

/* the initializer at the cursor, inside its designation if it has one */
static AST **cur_slot(Cursor *c) {
    AST **slot = &c->list->items[c->pos];
    if ((*slot)->kind == N_DESIGNATED) {
        return &(*slot)->u.desig.init;
    }
    return slot;
}

/* the next designator of the item at the cursor still to apply, or NULL */
static AST *cur_designator(Cursor *c) {
    AST *item = c->list->items[c->pos];
    if (item->kind == N_DESIGNATED && c->path_pos < item->u.desig.path->len) {
        return item->u.desig.path->items[c->path_pos];
    }
    return NULL;
}

/* a designation none of which has been applied: it addresses the brace level, not an elided sub-aggregate */
static int cur_is_fresh_designation(Cursor *c) {
    return c->list->items[c->pos]->kind == N_DESIGNATED && c->path_pos == 0;
}

static void cur_advance(Cursor *c) {
    c->pos++;
    c->path_pos = 0;
}

/* `top`: the list belongs to this object's own braces, so designators address it */
static void init_object(Type *t, Cursor *c, int is_static, long *count_out, int top);

/* resolve `[expr]` to an index; -1 if it is not usable */
static long designated_index(AST *d, Type *array) {
    long v;
    if (d->kind != N_DESIG_INDEX) {
        diag_error(d->loc, "field designator cannot initialize an array");
        return -1;
    }
    sema_rvalue(&d->u.cast.expr);
    if (!type_is_integer(d->u.cast.expr->type) || !sema_const_eval(d->u.cast.expr, &v)) {
        diag_error(d->loc, "array designator is not an integer constant expression");
        return -1;
    }
    if (v < 0 || (array->length >= 0 && v >= array->length)) {
        diag_error(d->loc, "array designator index %ld is out of range", v);
        return -1;
    }
    return (long)v;
}

/* resolve `.name` to a member index; -1 if there is none */
static long designated_member(AST *d, Record *r) {
    size_t i;
    if (d->kind != N_DESIG_MEMBER) {
        diag_error(d->loc, "array designator cannot initialize %s", type_kind_name(r->kind));
        return -1;
    }
    for (i = 0; i < r->nmembers; i++) {
        if (r->members[i].name && strcmp(r->members[i].name, d->u.name) == 0) {
            return (long)i;
        }
    }
    diag_error(d->loc, "%s %s has no member named '%s'", type_kind_name(r->kind), r->tag ? r->tag : "", d->u.name);
    return -1;
}

static void init_scalar(AST **slot, Type *t, int is_static) {
    AST *init = *slot;
    if (init->kind == N_INIT_LIST) {
        if (init->u.list->len == 0) {
            diag_error(init->loc, "empty initializer for scalar");
            return;
        }
        if (init->u.list->len > 1) {
            diag_warning(init->loc, "excess elements in scalar initializer");
        }
        init_scalar(&init->u.list->items[0], t, is_static);
        return;
    }
    sema_rvalue(slot);
    convert_assign(slot, t, "initializing");
    if (is_static && !is_constant_initializer(*slot)) {
        diag_error((*slot)->loc, "initializer element is not a compile-time constant");
    }
}

/* `char s[] = "..."`: returns 1 if the string was used */
static int init_char_array_from_string(Type **pt, AST **slot) {
    Type *t = *pt;
    AST *init = *slot;
    size_t n;

    if (init->kind != N_STRING || t->kind != TY_ARRAY || type_size(t->base) != sema_string_width(init->u.text)) {
        return 0;
    }
    sema_expr(slot);
    n = (size_t)init->type->length;   /* bytes plus the terminating NUL */
    if (t->length < 0) {
        *pt = type_array(t->base, (long)n);
    } else if ((long)n > t->length + 1) {
        diag_error(init->loc, "initializer string for a char array of %ld is too long", t->length);
    }
    return 1;
}

/* a fully braced initializer for `t` */
static void init_braced(Type **pt, AST *list, int is_static) {
    Cursor c;
    long count = 0;

    c.list = list->u.list;
    c.pos = 0;
    c.path_pos = 0;
    init_object(*pt, &c, is_static, &count, 1);
    if (c.pos < c.list->len) {
        diag_warning(c.list->items[c.pos]->loc, "excess elements in %s initializer",
                     (*pt)->kind == TY_ARRAY ? "array" : type_kind_name((*pt)->kind));
    }
    if ((*pt)->kind == TY_ARRAY && (*pt)->length < 0) {
        *pt = type_array((*pt)->base, count);
    }
}

/* one element or member, taken from the cursor; braces may be elided */
static void init_elem(Type *t, Cursor *c, int is_static) {
    AST **slot = cur_slot(c);
    Type *elem = t;

    if (cur_designator(c)) {
        /* the designation continues into this element */
        if (t->kind == TY_ARRAY || type_is_record(t)) {
            init_object(t, c, is_static, NULL, 0);
        } else {
            diag_error(cur_designator(c)->loc, "designator in initializer for scalar type %s", ts(t));
            cur_advance(c);
        }
        return;
    }
    if ((*slot)->kind == N_INIT_LIST) {
        cur_advance(c);
        if (type_is_scalar(t)) {
            init_scalar(slot, t, is_static);
        } else {
            init_braced(&elem, *slot, is_static);
        }
        return;
    }
    if (t->kind == TY_ARRAY && (*slot)->kind == N_STRING && type_size(t->base) == sema_string_width((*slot)->u.text)) {
        cur_advance(c);
        init_char_array_from_string(&elem, slot);
        return;
    }
    if (t->kind == TY_ARRAY || type_is_record(t)) {
        /* brace elision: the aggregate takes as many items as it needs */
        if (type_is_record(t) && (*slot)->kind != N_INIT_LIST) {
            /* a struct expression initialises the whole member */
            Type *et = sema_rvalue(slot);
            if (type_is_record(et) && type_compatible_unqual(et, t)) {
                cur_advance(c);
                if (is_static) {
                    diag_error((*slot)->loc, "initializer element is not a compile-time constant");
                }
                return;
            }
        }
        init_object(t, c, is_static, NULL, 0);
        return;
    }
    cur_advance(c);
    init_scalar(slot, t, is_static);
}

static void init_object(Type *t, Cursor *c, int is_static, long *count_out, int top) {
    long n = 0;

    if (t->kind == TY_ARRAY) {
        long count = 0;
        while (c->pos < c->list->len) {
            AST *d;
            if (!top && cur_is_fresh_designation(c)) {
                break;
            }
            d = cur_designator(c);
            if (d) {
                long k = designated_index(d, t);
                if (k < 0) {
                    cur_advance(c);
                    continue;
                }
                n = k;
                c->path_pos++;
            } else if (t->length >= 0 && n >= t->length) {
                break;
            }
            init_elem(t->base, c, is_static);
            n++;
            if (n > count) {
                count = n;
            }
        }
        if (count_out) {
            *count_out = count;
        }
        return;
    }
    if (t->kind == TY_STRUCT) {
        Record *r = t->record;
        size_t i = 0;
        while (c->pos < c->list->len) {
            AST *d;
            if (!top && cur_is_fresh_designation(c)) {
                break;
            }
            d = cur_designator(c);
            if (d) {
                long k = designated_member(d, r);
                if (k < 0) {
                    cur_advance(c);
                    continue;
                }
                i = (size_t)k;
                c->path_pos++;
            } else if (i >= r->nmembers) {
                break;
            } else if (!r->members[i].name && r->members[i].bits >= 0) {
                i++;    /* unnamed bit-field */
                continue;
            }
            init_elem(r->members[i].type, c, is_static);
            i++;
        }
        return;
    }
    if (t->kind == TY_UNION) {
        Record *r = t->record;
        if (r->nmembers && c->pos < c->list->len) {
            AST *d;
            size_t i = 0;
            if (!top && cur_is_fresh_designation(c)) {
                return;
            }
            d = cur_designator(c);
            if (d) {
                long k = designated_member(d, r);
                if (k < 0) {
                    cur_advance(c);
                    return;
                }
                i = (size_t)k;
                c->path_pos++;
            }
            init_elem(r->members[i].type, c, is_static);
        }
        return;
    }
    if (c->pos < c->list->len) {
        init_elem(t, c, is_static);
    }
}

static void sema_initializer(AST **slot, Type **pt, int is_static) {
    Type *t = *pt;
    AST *init = *slot;

    if (init->kind == N_INIT_LIST) {
        if (type_is_scalar(t)) {
            init_scalar(slot, t, is_static);
            return;
        }
        if (!type_is_complete(t) && !(t->kind == TY_ARRAY && t->length < 0 && type_is_complete(t->base))) {
            diag_error(init->loc, "initializing an object of incomplete type %s", ts(t));
            return;
        }
        init_braced(pt, init, is_static);
        return;
    }
    if (init_char_array_from_string(pt, slot)) {
        return;
    }
    if (t->kind == TY_ARRAY) {
        diag_error(init->loc, "array initializer must be an initializer list or string literal");
        sema_rvalue(slot);
        return;
    }
    if (type_is_record(t)) {
        Type *et = sema_rvalue(slot);
        if (!type_compatible_unqual(et, t)) {
            diag_error(init->loc, "initializing %s from incompatible type %s", ts(t), ts(et));
        } else if (is_static) {
            diag_error(init->loc, "initializer element is not a compile-time constant");
        }
        return;
    }
    init_scalar(slot, t, is_static);
}

/* declarations */

static void report_conflict(Symbol *sym, Loc loc, const char *name) {
    diag_error(loc, "conflicting types for '%s'", name);
    diag_note(sym->loc, "previous declaration is here");
}

static int only_attributes(const ASTList *specs) {
    size_t i;
    for (i = 0; i < specs->len; i++) {
        if (specs->items[i]->kind != N_ATTRIBUTE) {
            return 0;
        }
    }
    return specs->len > 0;
}

static void sema_declaration(AST *decl, int file_scope) {
    Specs sp;
    size_t i;

    if (decl->u.decl.declarators->len == 0 && only_attributes(decl->u.decl.specs)) {
        /* `__attribute__((fallthrough));`: an attribute declaration, nothing to do */
        return;
    }
    sema_specs(decl->u.decl.specs, &sp, 1, decl->loc);
    if (decl->u.decl.declarators->len == 0) {
        if (!sp.only_tag) {
            diag_warning(decl->loc, "declaration does not declare anything");
        }
        return;
    }
    if (file_scope && (sp.storage == STORAGE_AUTO || sp.storage == STORAGE_REGISTER)) {
        diag_error(decl->loc, "illegal storage class on file-scoped variable");
        sp.storage = STORAGE_NONE;
    }

    for (i = 0; i < decl->u.decl.declarators->len; i++) {
        AST *id = decl->u.decl.declarators->items[i];
        AST **init = &id->u.init_decl.init;
        const char *name = NULL;
        Type *t = sema_declarator(id->u.init_decl.declarator, sp.type, &name);
        Symbol *sym = id->sym;
        int is_static;

        id->type = t;
        if (!sym || !name) {
            diag_error(id->loc, "internal error: declarator was not declared");
            continue;
        }
        sym->storage = sp.storage;

        if (sp.is_typedef) {
            if (sym->kind != SYM_TYPEDEF) {
                diag_error(id->loc, "redefinition of '%s' as a typedef", name);
                diag_note(sym->loc, "previous declaration is here");
                continue;
            }
            if (sym->type && sym->decl != id) {
                if (!type_compatible(sym->type, t)) {
                    diag_error(id->loc, "typedef redefinition of '%s' with different types (%s vs %s)", name, ts(t), ts(sym->type));
                    diag_note(sym->loc, "previous definition is here");
                }
                continue;
            }
            if (*init) {
                diag_error(id->loc, "illegal initializer (only variables can be initialized)");
            }
            sym->type = t;
            continue;
        }

        if (sym->kind == SYM_TYPEDEF || sym->kind == SYM_ENUMERATOR) {
            diag_error(id->loc, "redefinition of '%s' as a different kind of symbol", name);
            diag_note(sym->loc, "previous definition is here");
            continue;
        }

        if (t->kind == TY_FUNC) {
            if (sym->kind != SYM_FUNCTION) {
                diag_error(id->loc, "redefinition of '%s' as a different kind of symbol", name);
                diag_note(sym->loc, "previous definition is here");
                continue;
            }
            if (*init) {
                diag_error(id->loc, "function '%s' cannot have an initializer", name);
            }
            if (sp.storage != STORAGE_NONE && sp.storage != STORAGE_STATIC && sp.storage != STORAGE_EXTERN) {
                diag_error(id->loc, "invalid storage class for function '%s'", name);
            }
            if (sym->type) {
                if (!type_compatible(sym->type, t)) {
                    report_conflict(sym, id->loc, name);
                } else {
                    sym->type = type_composite(sym->type, t);
                }
                if (sym->decl != id) {
                    sym->storage = sym->storage == STORAGE_STATIC ? STORAGE_STATIC : sp.storage;
                }
            } else {
                sym->type = t;
            }
            continue;
        }

        /* an object */
        if (sym->kind != SYM_VARIABLE && sym->kind != SYM_PARAM) {
            diag_error(id->loc, "redefinition of '%s' as a different kind of symbol", name);
            diag_note(sym->loc, "previous definition is here");
            continue;
        }
        if (t->kind == TY_VOID) {
            diag_error(id->loc, "variable '%s' has incomplete type void", name);
            continue;
        }
        if (sym->kind == SYM_PARAM) {
            /* a K&R parameter declaration */
            if (sym->decl != id) {
                diag_error(id->loc, "redeclaration of parameter '%s'", name);
                continue;
            }
            if (*init) {
                diag_error(id->loc, "parameter '%s' cannot have an initializer", name);
            }
            if (sp.storage != STORAGE_NONE && sp.storage != STORAGE_REGISTER) {
                diag_error(id->loc, "invalid storage class for parameter '%s'", name);
            }
            sym->type = adjust_param(t);
            continue;
        }
        if (sym->type && sym->decl != id) {
            /* a redeclaration in the same scope */
            if (!file_scope && !(sp.storage == STORAGE_EXTERN && sym->storage == STORAGE_EXTERN)) {
                diag_error(id->loc, "redefinition of '%s'", name);
                diag_note(sym->loc, "previous definition is here");
                continue;
            }
            if (!type_compatible(sym->type, t)) {
                report_conflict(sym, id->loc, name);
                continue;
            }
            t = type_composite(sym->type, t);
            if (sp.storage == STORAGE_EXTERN) {
                sym->storage = sym->storage == STORAGE_NONE ? STORAGE_NONE : sym->storage;
            } else if (sym->storage == STORAGE_EXTERN) {
                sym->storage = sp.storage;
            } else if (sp.storage == STORAGE_STATIC && sym->storage != STORAGE_STATIC) {
                diag_error(id->loc, "static declaration of '%s' follows non-static declaration", name);
            }
        }
        is_static = file_scope || sp.storage == STORAGE_STATIC;
        if (*init) {
            if (sp.storage == STORAGE_EXTERN && !file_scope) {
                diag_error(id->loc, "'extern' variable '%s' cannot have an initializer", name);
            }
            if (sym->def && sym->def != id) {
                diag_error(id->loc, "redefinition of '%s'", name);
                diag_note(sym->def->loc, "previous definition is here");
            } else {
                sym->def = id;
            }
            /* the name is in scope in its own initializer: `T *p = f(sizeof(*p))` */
            sym->type = t;
            sema_initializer(init, &t, is_static);
        }
        if (!type_is_complete(t) && sp.storage != STORAGE_EXTERN) {
            if (t->kind == TY_ARRAY && t->length < 0 && file_scope) {
                /* a tentative definition; it becomes one element if never completed */
            } else {
                diag_error(id->loc, "variable '%s' has incomplete type %s", name, ts(t));
            }
        }
        sym->type = t;
    }
}

/* statements */

static void check_condition(AST **slot) {
    Type *t = sema_rvalue(slot);
    if (!type_is_scalar(t)) {
        diag_error((*slot)->loc, "condition has non-scalar type %s", ts(t));
    }
}

static void switch_case(AST *stmt) {
    SwitchCtx *sw = switch_ctx;
    long v;
    size_t i;

    if (!sw) {
        diag_error(stmt->loc, "'case' statement not in a switch statement");
        return;
    }
    sema_rvalue(&stmt->u.stmt.expr);
    if (!type_is_integer(stmt->u.stmt.expr->type) || !sema_const_eval(stmt->u.stmt.expr, &v)) {
        diag_error(stmt->loc, "case label is not an integer constant expression");
        return;
    }
    v = sema_fit(v, sw->type);
    for (i = 0; i < sw->nvalues; i++) {
        if (sw->values[i] == v) {
            diag_error(stmt->loc, "duplicate case value %ld", v);
            return;
        }
    }
    if (sw->nvalues == sw->cap) {
        sw->cap = sw->cap ? sw->cap * 2 : 16;
        sw->values = realloc(sw->values, sw->cap * sizeof(*sw->values));
        if (!sw->values) {
            fatal("out of memory");
        }
    }
    sw->values[sw->nvalues++] = v;
}

static void sema_stmt(AST *s) {
    size_t i;

    if (!s) {
        return;
    }
    switch (s->kind) {
        case N_COMPOUND:
            for (i = 0; i < s->u.block.items->len; i++) {
                AST *item = s->u.block.items->items[i];
                if (item->kind == N_DECLARATION) {
                    sema_declaration(item, 0);
                } else {
                    sema_stmt(item);
                }
            }
            break;

        case N_EXPR_STMT:
            if (s->u.stmt.expr) {
                sema_rvalue(&s->u.stmt.expr);
            }
            break;

        case N_IF:
            check_condition(&s->u.cond.cond);
            sema_stmt(s->u.cond.then);
            sema_stmt(s->u.cond.otherwise);
            break;

        case N_WHILE:
            check_condition(&s->u.stmt.expr);
            loop_depth++;
            sema_stmt(s->u.stmt.body);
            loop_depth--;
            break;

        case N_DO:
            loop_depth++;
            sema_stmt(s->u.stmt.body);
            loop_depth--;
            check_condition(&s->u.stmt.expr);
            break;

        case N_FOR:
            if (s->u.loop.init) {
                sema_rvalue(&s->u.loop.init);
            }
            if (s->u.loop.cond) {
                check_condition(&s->u.loop.cond);
            }
            if (s->u.loop.step) {
                sema_rvalue(&s->u.loop.step);
            }
            loop_depth++;
            sema_stmt(s->u.loop.body);
            loop_depth--;
            break;

        case N_SWITCH: {
            SwitchCtx sw;
            Type *t = sema_rvalue(&s->u.stmt.expr);
            memset(&sw, 0, sizeof(sw));
            if (!type_is_integer(t)) {
                diag_error(s->loc, "switch condition has non-integer type %s", ts(t));
                sw.type = int_type();
            } else {
                sw.type = promote_operand(&s->u.stmt.expr);
            }
            sw.outer = switch_ctx;
            switch_ctx = &sw;
            sema_stmt(s->u.stmt.body);
            switch_ctx = sw.outer;
            free(sw.values);
            break;
        }

        case N_CASE:
            switch_case(s);
            sema_stmt(s->u.stmt.body);
            break;

        case N_DEFAULT:
            if (!switch_ctx) {
                diag_error(s->loc, "'default' statement not in a switch statement");
            } else if (switch_ctx->has_default) {
                diag_error(s->loc, "multiple default labels in one switch");
            } else {
                switch_ctx->has_default = 1;
            }
            sema_stmt(s->u.stmt.body);
            break;

        case N_LABEL:
            if (s->sym && s->sym->decl != s) {
                diag_error(s->loc, "redefinition of label '%s'", s->u.stmt.name);
                diag_note(s->sym->loc, "previous definition is here");
            }
            sema_stmt(s->u.stmt.body);
            break;

        case N_GOTO:
            cur_gotos = ast_list_push(cur_gotos, s);
            break;

        case N_BREAK:
            if (!loop_depth && !switch_ctx) {
                diag_error(s->loc, "'break' statement not in a loop or switch statement");
            }
            break;

        case N_CONTINUE:
            if (!loop_depth) {
                diag_error(s->loc, "'continue' statement not in a loop statement");
            }
            break;

        case N_RETURN:
            if (s->u.stmt.expr) {
                Type *t = sema_rvalue(&s->u.stmt.expr);
                if (cur_return->kind == TY_VOID) {
                    if (t->kind != TY_VOID) {
                        diag_error(s->loc, "void function should not return a value");
                    }
                } else {
                    convert_assign(&s->u.stmt.expr, cur_return, "returning");
                }
            } else if (cur_return->kind != TY_VOID) {
                diag_warning(s->loc, "non-void function should return a value");
            }
            break;

        default:
            diag_error(s->loc, "internal error: %s is not a statement", node_kind_name(s->kind));
            break;
    }
}

/* functions */

static void sema_function(AST *fdef) {
    Specs sp;
    const char *name = NULL;
    AST *declarator = fdef->u.func.declarator;
    AST *fn = ast_declarator_innermost(declarator);
    Symbol *sym = fdef->sym;
    Type *t;
    size_t i;

    sema_specs(fdef->u.func.specs, &sp, 1, fdef->loc);
    if (sp.storage != STORAGE_NONE && sp.storage != STORAGE_STATIC && sp.storage != STORAGE_EXTERN) {
        diag_error(fdef->loc, "invalid storage class for a function definition");
        sp.storage = STORAGE_NONE;
    }
    if (sp.is_inline && sp.storage == STORAGE_EXTERN) {
        /*
         * GNU89 `extern inline`: a definition that exists only to be inlined
         * and must not produce an external symbol (the macOS headers define
         * __sputc and the fd_set helpers this way in every unit). jcc never
         * inlines, so give it internal linkage: emitted once per unit, as a
         * local symbol, and only when something in the unit calls it.
         */
        sp.storage = STORAGE_STATIC;
    }
    defining_fn = fn;
    t = sema_declarator(declarator, sp.type, &name);
    defining_fn = NULL;
    if (t->kind != TY_FUNC || !fn || fn->kind != N_DECL_FUNC) {
        diag_error(fdef->loc, "'%s' is not a function declarator", name ? name : "");
        return;
    }
    fdef->type = t;

    /* K&R parameters: the declaration list gives the types, the identifier list the order */
    if (fn->u.declarator.kr) {
        ASTList *idents = fn->u.declarator.params;
        Param *params = idents->len ? xcalloc(idents->len, sizeof(*params)) : NULL;
        if (fdef->u.func.kr_decls) {
            for (i = 0; i < fdef->u.func.kr_decls->len; i++) {
                AST *d = fdef->u.func.kr_decls->items[i];
                size_t j;
                sema_declaration(d, 0);
                for (j = 0; j < d->u.decl.declarators->len; j++) {
                    AST *id = d->u.decl.declarators->items[j];
                    if (id->sym && id->sym->kind != SYM_PARAM) {
                        diag_error(id->loc, "'%s' is not a parameter of '%s'", ast_declarator_name(id->u.init_decl.declarator), name);
                    }
                }
            }
        }
        for (i = 0; i < idents->len; i++) {
            AST *ident = idents->items[i];
            Symbol *ps = ident->sym;
            if (ps && !ps->type) {
                ps->type = int_type();  /* an undeclared K&R parameter is an int */
            }
            params[i].name = xstrdup(ident->u.name);
            params[i].type = ps ? ps->type : int_type();
            ident->type = params[i].type;
            if (!type_is_complete(params[i].type)) {
                diag_error(ident->loc, "parameter '%s' has incomplete type %s", ident->u.name, ts(params[i].type));
            }
        }
        t = type_func(t->base, params, idents->len, 0, 0);
        fdef->type = t;
    } else {
        for (i = 0; i < t->nparams; i++) {
            AST *p = fn->u.declarator.params->items[i];
            if (!t->params[i].name) {
                diag_error(p->loc, "parameter name omitted");
            } else if (!type_is_complete(t->params[i].type)) {
                /* a declaration may leave it incomplete; the definition cannot */
                diag_error(p->loc, "parameter '%s' has incomplete type %s", t->params[i].name, ts(t->params[i].type));
            }
        }
        /* the prototype's parameters also came through sema_declarator, which typed them */
    }

    if (t->base->kind != TY_VOID && !type_is_complete(t->base)) {
        diag_error(fdef->loc, "function '%s' returns incomplete type %s", name, ts(t->base));
    }

    if (sym) {
        if (sym->type) {
            if (!type_compatible(sym->type, t)) {
                report_conflict(sym, fdef->loc, name);
            } else {
                t = type_composite(sym->type, t);
                /* the definition's parameter names win */
                if (sym->type->has_proto == t->has_proto) {
                    t = fdef->type;
                }
            }
            if (sym->def && sym->def != fdef) {
                diag_error(fdef->loc, "redefinition of '%s'", name);
                diag_note(sym->def->loc, "previous definition is here");
            }
            if (sym->storage != STORAGE_STATIC) {
                sym->storage = sp.storage;
            }
        } else {
            sym->storage = sp.storage;
        }
        sym->type = t;
        fdef->type = t;
    }

    cur_return = t->base;
    cur_gotos = NULL;
    loop_depth = 0;
    switch_ctx = NULL;
    sema_stmt(fdef->u.func.body);

    /* labels may be used before they are defined, so gotos resolve last */
    if (cur_gotos) {
        for (i = 0; i < cur_gotos->len; i++) {
            AST *g = cur_gotos->items[i];
            Symbol *label = scope_lookup(fdef->u.func.scope, NS_LABEL, g->u.stmt.name);
            if (!label) {
                diag_error(g->loc, "use of undeclared label '%s'", g->u.stmt.name);
            }
            g->sym = label;
        }
        free(cur_gotos->items);
        free(cur_gotos);
        cur_gotos = NULL;
    }
    cur_return = NULL;
}

/* the translation unit */

int sema(AST *tu, SymTab *st) {
    size_t i;

    cur_symtab = st;
    if (tu->kind != N_TRANSLATION_UNIT) {
        diag_error(tu->loc, "internal error: expected a translation unit");
        return diag_error_count();
    }
    for (i = 0; i < tu->u.list->len; i++) {
        AST *item = tu->u.list->items[i];
        if (item->kind == N_FUNCTION_DEF) {
            sema_function(item);
        } else if (item->kind == N_DECLARATION) {
            sema_declaration(item, 1);
        } else {
            diag_error(item->loc, "internal error: %s at file scope", node_kind_name(item->kind));
        }
    }
    cur_symtab = NULL;
    return diag_error_count();
}

/* builtin functions */

/*
 * Signatures are written as `ret(params)` with one letter per type:
 *   v void   c char   i int   u unsigned   l long   z size_t
 *   h unsigned short   w unsigned int   q unsigned long
 *   d double   A va_list   p void *   P const void *
 *   Y Z X float, double and long double _Complex
 *   s char *   S const char *   . variadic
 * `(v)` is an empty prototype.
 */
static const struct BuiltinFn {
    const char *name;
    const char *sig;
} builtins[] = {
    { "__builtin_va_start", "v(A.)" },
    { "__builtin_va_end", "v(A)" },
    { "__builtin_va_copy", "v(AA)" },
    { "__builtin_object_size", "z(Pi)" },
    { "__builtin___memcpy_chk", "p(pPzz)" },
    { "__builtin___memmove_chk", "p(pPzz)" },
    { "__builtin___memset_chk", "p(pizz)" },
    { "__builtin___strcpy_chk", "s(sSz)" },
    { "__builtin___stpcpy_chk", "s(sSz)" },
    { "__builtin___strncpy_chk", "s(sSzz)" },
    { "__builtin___stpncpy_chk", "s(sSzz)" },
    { "__builtin___strcat_chk", "s(sSz)" },
    { "__builtin___strncat_chk", "s(sSzz)" },
    { "__builtin___strlcpy_chk", "z(sSzz)" },
    { "__builtin___strlcat_chk", "z(sSzz)" },
    { "__builtin___snprintf_chk", "i(szizS.)" },
    { "__builtin___sprintf_chk", "i(sizS.)" },
    { "__builtin___vsnprintf_chk", "i(szizSA)" },
    { "__builtin___vsprintf_chk", "i(sizSA)" },
    { "__builtin_bswap16", "h(h)" },
    { "__builtin_bswap32", "w(w)" },
    { "__builtin_bswap64", "q(q)" },
    { "__builtin_expect", "l(ll)" },
    { "__builtin_alloca", "p(z)" },
    { "__builtin_memcpy", "p(pPz)" },
    { "__builtin_memmove", "p(pPz)" },
    { "__builtin_memset", "p(piz)" },
    { "__builtin_memcmp", "i(PPz)" },
    { "__builtin_strlen", "z(S)" },
    { "__builtin_strcmp", "i(SS)" },
    { "__builtin_abort", "v(v)" },
    /* the macOS SDK's math.h classification helpers and INFINITY/NAN/HUGE_VAL use these */
    { "__builtin_fabsf", "f(f)" },
    { "__builtin_fabs", "d(d)" },
    { "__builtin_fabsl", "D(D)" },
    { "__builtin_inff", "f(v)" },
    { "__builtin_inf", "d(v)" },
    { "__builtin_infl", "D(v)" },
    { "__builtin_huge_valf", "f(v)" },
    { "__builtin_huge_val", "d(v)" },
    { "__builtin_huge_vall", "D(v)" },
    { "__builtin_nanf", "f(S)" },
    { "__builtin_nan", "d(S)" },
    { "__builtin_nanl", "D(S)" },
    { "__builtin_creal", "d(Z)" },
    { "__builtin_crealf", "f(Y)" },
    { "__builtin_creall", "D(X)" },
    { "__builtin_cimag", "d(Z)" },
    { "__builtin_cimagf", "f(Y)" },
    { "__builtin_cimagl", "D(X)" },
    { "__builtin_conj", "Z(Z)" },
    { "__builtin_conjf", "Y(Y)" },
    { "__builtin_conjl", "X(X)" },
    { "__builtin_trap", "v(v)" },
    { "__builtin_unreachable", "v(v)" },
    /* type generic: the operands keep their own types, the third is a pointer to the result */
    { "__builtin_add_overflow", "i(.)" },
    { "__builtin_sub_overflow", "i(.)" },
    { "__builtin_mul_overflow", "i(.)" },
    { "__builtin_clz", "i(u)" },
    { "__builtin_clzl", "i(U)" },
    { "__builtin_clzll", "i(q)" },
    { "__builtin_ctz", "i(u)" },
    { "__builtin_ctzl", "i(U)" },
    { "__builtin_ctzll", "i(q)" },
    { "__sync_synchronize", "v(v)" },
    /* the libc functions GCC knows as builtins (codegen calls the real ones) and prefetch, a no-op */
    { "__builtin_memchr", "p(Piz)" },
    { "__builtin_strncmp", "i(SSz)" },
    { "__builtin_strcpy", "s(sS)" },
    { "__builtin_strncpy", "s(sSz)" },
    { "__builtin_strcat", "s(sS)" },
    { "__builtin_strncat", "s(sSz)" },
    { "__builtin_strchr", "s(Si)" },
    { "__builtin_strrchr", "s(Si)" },
    { "__builtin_strstr", "s(SS)" },
    { "__builtin_printf", "i(S.)" },
    { "__builtin_sprintf", "i(sS.)" },
    { "__builtin_snprintf", "i(szS.)" },
    { "__builtin_puts", "i(S)" },
    { "__builtin_putchar", "i(i)" },
    { "__builtin_malloc", "p(z)" },
    { "__builtin_calloc", "p(zz)" },
    { "__builtin_realloc", "p(pz)" },
    { "__builtin_free", "v(p)" },
    { "__builtin_exit", "v(i)" },
    { "__builtin_abs", "i(i)" },
    { "__builtin_labs", "l(l)" },
    { "__builtin_llabs", "x(x)" },
    { "__builtin_ffs", "i(i)" },
    { "__builtin_ffsl", "i(l)" },
    { "__builtin_ffsll", "i(x)" },
    { "__builtin_copysign", "d(dd)" },
    { "__builtin_copysignf", "f(ff)" },
    { "__builtin_copysignl", "D(DD)" },
    { "__builtin_sqrt", "d(d)" },
    { "__builtin_sqrtf", "f(f)" },
    { "__builtin_floor", "d(d)" },
    { "__builtin_floorf", "f(f)" },
    { "__builtin_ceil", "d(d)" },
    { "__builtin_ceilf", "f(f)" },
    { "__builtin_fmod", "d(dd)" },
    { "__builtin_fmodf", "f(ff)" },
    { "__builtin_bzero", "v(pz)" },
    { "__builtin_bcopy", "v(Ppz)" },
    { "__builtin_prefetch", "v(P.)" },
    { NULL, NULL }
};

/*
 * `__atomic_load_n(p, order)` and `__atomic_store_n(p, v, order)` are
 * generic in the pointee type, so they are not in the table: the call
 * is checked here and the function symbol only exists to be called.
 */
static Type *sema_atomic(AST *e, int is_store) {
    AST **f = &e->u.call.func;
    ASTList *args = e->u.call.args;
    Symbol *sym;
    Type *pt;
    Type *pointee;
    size_t i;

    sym = symtab_declare(cur_symtab, NS_ORDINARY, SYM_FUNCTION, (*f)->u.name, NULL, (*f)->loc);
    if (!sym->type) {
        sym->type = type_func(int_type(), NULL, 0, 0, 0);
    }
    (*f)->sym = sym;
    sema_rvalue(f);
    for (i = 0; i < args->len; i++) {
        sema_rvalue(&args->items[i]);
    }
    e->lvalue = 0;
    if (args->len != (is_store ? 3u : 2u)) {
        diag_error(e->loc, "'%s' takes %d arguments", sym->name, is_store ? 3 : 2);
        return error_type(e);
    }
    pt = args->items[0]->type;
    if (pt->kind != TY_POINTER || !type_is_scalar(pt->base)) {
        diag_error(args->items[0]->loc, "'%s' needs a pointer to a scalar, not %s", sym->name, ts(pt));
        return error_type(e);
    }
    pointee = type_unqualified(pt->base);
    if (is_store) {
        convert_assign(&args->items[1], pointee, "storing a value of type");
        e->type = type_basic(TY_VOID);
    } else {
        e->type = pointee;
    }
    return e->type;
}

static Type *sig_type(char c) {
    switch (c) {
        case 'v': return type_basic(TY_VOID);
        case 'c': return type_basic(TY_CHAR);
        case 'i': return type_basic(TY_INT);
        case 'u': return type_basic(TY_UINT);
        case 'l': return type_basic(TY_LONG);
        case 'z': return type_size_t();
        case 'h': return type_basic(TY_USHORT);
        case 'w': return type_basic(TY_UINT);
        case 'q': return type_basic(TY_ULLONG);
        case 'x': return type_basic(TY_LLONG);
        case 'U': return type_basic(TY_ULONG);
        case 'd': return type_basic(TY_DOUBLE);
        case 'f': return type_basic(TY_FLOAT);
        case 'D': return type_basic(TY_LDOUBLE);
        case 'Y': return type_basic(TY_CFLOAT);
        case 'Z': return type_basic(TY_CDOUBLE);
        case 'X': return type_basic(TY_CLDOUBLE);
        case 'A': return adjust_param(type_basic(TY_VA_LIST));
        case 'p': return type_pointer(type_basic(TY_VOID));
        case 'P': return type_pointer(type_qualify(type_basic(TY_VOID), TQ_CONST));
        case 's': return type_pointer(type_basic(TY_CHAR));
        case 'S': return type_pointer(type_qualify(type_basic(TY_CHAR), TQ_CONST));
        default: return NULL;
    }
}

static Type *builtin_type(const char *name) {
    const struct BuiltinFn *b;
    const char *p;
    Type *ret;
    Param *params;
    size_t n = 0;
    int variadic = 0;

    if (strcmp(name, "alloca") == 0) {
        name = "__builtin_alloca";   /* a builtin under its own name too, as in GCC, so no declaration is needed */
    }
    if (strncmp(name, "__builtin_", 10) != 0 && strncmp(name, "__sync_", 7) != 0) {
        return NULL;
    }
    for (b = builtins; b->name; b++) {
        if (strcmp(b->name, name) == 0) {
            break;
        }
    }
    if (!b->name) {
        return NULL;
    }
    p = b->sig;
    ret = sig_type(*p++);
    p++;    /* ( */
    params = xcalloc(strlen(p), sizeof(*params));
    while (*p && *p != ')') {
        if (*p == '.') {
            variadic = 1;
        } else if (!(*p == 'v' && p[1] == ')')) {
            params[n].name = NULL;
            params[n].type = sig_type(*p);
            n++;
        }
        p++;
    }
    return type_func(ret, params, n, variadic, 1);
}

int sema_const_eval_float(AST *expr, long double *value) {
    return float_eval(expr, value);
}

size_t sema_string_width(const char *text) {
    return literal_is_wide(text) ? wide_width() : 1;
}

size_t sema_string_bytes(const char *text, char **out) {
    size_t w = sema_string_width(text);
    size_t n = decode_literal(text, NULL, NULL, w);
    char *buf = xcalloc(n + 1, w);     /* zeroed: the terminator is in place */
    decode_literal(text, NULL, buf, w);
    *out = buf;
    return n * w;
}
