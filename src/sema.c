#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include "sema.h"
#include "diag.h"
#include "type.h"
#include "arch.h"
#include "target.h"
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

/* the function being checked, and (GNU nested functions) the ones it is defined in, innermost last */
static Symbol *fn_stack[64];
static Scope *fn_scope_stack[64];
static int fn_depth = 0;
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

/* analysing the initializer of an object with static storage: a compound literal in it is static too */
static int init_static_ctx = 0;
static int ncompound_lits = 0;     /* numbers the static ones' names */

/* a specifier list, combined */
typedef struct Specs {
    Type *type;
    StorageKind storage;
    int is_inline;
    int is_typedef;
    int packed;         /* __attribute__((packed)) seen */
    size_t aligned;     /* __attribute__((aligned(n))) or _Alignas seen */
    int has_alignas;    /* _Alignas, which a typedef may not have */
    int sso;            /* scalar_storage_order: 1 big-endian, 2 little-endian, 0 none */
    int auto_type;      /* __auto_type: `type` is a placeholder, the initializer gives the type */
    int only_tag;       /* the list is just `struct S` or `enum E` (with or without body) */
    int is_thread;      /* _Thread_local or __thread */
} Specs;

static Type *sema_expr(AST **slot);
static Type *sema_rvalue(AST **slot);
static void sema_specs(ASTList *specs, Specs *out, int allow_storage, Loc loc);
static Type *sema_declarator(AST *d, Type *base, const char **name_out);
static Type *sema_type_name(AST *tn);
static void sema_declaration(AST *decl, int file_scope);
static void sema_stmt(AST *stmt);
static void sema_function(AST *fdef);
static void sema_initializer(AST **slot, Type **ptype, int is_static);
static int static_assertion(AST *decl);
static int float_eval(AST *e, long double *out);
static Type *builtin_type(const char *name);
static Type *sema_atomic(AST *e, const AtomicBuiltin *ab);
static Type *sema_shuffle(AST *e);
static int check_modifiable(AST *e, const char *what);
static const char *find_attr(const char *text, const char *word);

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

/* the _FloatN suffix of a floating literal (f32, f64, f128, f32x, f64x, and GNU q for __float128), as the type; NULL if none */
static Type *floatn_suffix(const char *text) {
    size_t n = strlen(text);
    const char *p;
    if (n && (text[n - 1] == 'q' || text[n - 1] == 'Q')) {
        return type_basic(TY_FLOAT128);
    }
    for (p = text + n; p > text && (isdigit((unsigned char)p[-1]) || p[-1] == 'x' || p[-1] == 'X'); p--) {
    }
    if (p == text || (p[-1] != 'f' && p[-1] != 'F') || !isdigit((unsigned char)*p)) {
        return NULL;
    }
    if (strcmp(p, "128") == 0) return type_basic(TY_FLOAT128);
    if (strcmp(p, "64x") == 0 || strcmp(p, "64X") == 0) return type_basic(TY_LDOUBLE);
    if (strcmp(p, "64") == 0 || strcmp(p, "32x") == 0 || strcmp(p, "32X") == 0) return type_basic(TY_DOUBLE);
    if (strcmp(p, "32") == 0) return type_basic(TY_FLOAT);
    return NULL;
}

static Type *float_literal_type(const char *text) {
    int is_float, is_long, is_imaginary;
    Type *t;
    size_t n = strlen(text);
    if (n > 2 && (text[n - 2] == 'd' || text[n - 2] == 'D')) {
        char c = (char)tolower((unsigned char)text[n - 1]);
        if (c == 'f' || c == 'd' || c == 'l') {
            return type_basic(c == 'f' ? TY_DECIMAL32 : c == 'd' ? TY_DECIMAL64 : TY_DECIMAL128);
        }
    }
    if ((t = floatn_suffix(text)) != NULL) {
        return t;
    }
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
        case N_SIZEOF_TYPE: {
            /* an alignment is constant even for a variable length array */
            Type *t = e->kind == N_SIZEOF_EXPR ? e->u.cast.expr->type : e->u.cast.type->type;
            if (!t || (!e->u.cast.align && type_is_vla(t))) {
                return 0;
            }
            *out = (long)(e->u.cast.align ? type_align(t) : type_size(t));
            return 1;
        }

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
            /* a thread-local object's address differs per thread: not a constant */
            return o->sym && is_static_storage(o->sym) && !o->sym->tls;
        case N_STRING:
            return 1;
        case N_COMPOUND_LIT:
            return o->sym != NULL;
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
        case N_LABEL_ADDR:
            return 1;
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

void sema_asm_constraint(const char *text, AsmConstraint *c) {
    int arm = strcmp(arch_info->name, "arm64") == 0;
    const char *p;
    memset(c, 0, sizeof(*c));
    c->match = -1;
    for (p = text; *p && *p != ','; p++) {
        switch (*p) {
            case '=': c->output = 1; break;
            case '+': c->output = 1; c->inout = 1; break;
            case '&': case '%': case '?': case '!': case ' ': break;
            case '*': if (p[1]) p++; break;     /* the next letter is a register preference only */
            case 'r': case 'l': case 'R': case 'U': case 'p': c->reg = 1; break;
            case 'q': c->reg = 1; break;
            case 'Q': if (arm) c->mem = 1; else c->reg = 1; break;
            case 'a': case 'b': case 'c': case 'd': case 'S': case 'D':
                if (arm) { c->unsupported = *p; } else { c->reg = 1; c->specific = *p; }
                break;
            case 'x': case 'Y': case 'v': c->reg = 1; c->fp = 1; break;
            case 'w': if (arm) { c->reg = 1; c->fp = 1; } else { c->unsupported = *p; } break;
            case 't': case 'u': if (arm) { c->unsupported = *p; } else { c->reg = 1; c->specific = *p; } break;
            case 'm': case 'o': case 'V': case '<': case '>': c->mem = 1; break;
            case 'i': case 'n': case 'I': case 'J': case 'K': case 'L': case 'M': case 'N': case 'O': case 'P':
            case 'e': case 'Z': case 's': case 'E': case 'F': case 'G': case 'H':
                c->imm = 1; break;
            case 'g': case 'X': c->reg = 1; c->mem = 1; c->imm = 1; break;
            case '[': {
                const char *e = strchr(p, ']');
                if (!e) {
                    c->unsupported = '[';
                    return;
                }
                c->match_name = p + 1;
                c->match_len = (size_t)(e - p - 1);
                p = e;
                break;
            }
            default:
                if (isdigit((unsigned char)*p)) {
                    c->match = (int)strtol(p, NULL, 10);
                    while (isdigit((unsigned char)p[1])) {
                        p++;
                    }
                } else if (!c->unsupported) {
                    c->unsupported = *p;
                }
                break;
        }
    }
}

/* an asm statement: its operands are checked as the constraints require */
static void sema_asm(AST *s) {
    ASTList *outs = s->u.asmst.outputs;
    ASTList *ins = s->u.asmst.inputs;
    size_t nouts = outs ? outs->len : 0;
    size_t i;
    for (i = 0; outs && i < outs->len; i++) {
        AST *op = outs->items[i];
        AsmConstraint c;
        char *text = NULL;
        sema_string_bytes(op->u.asmop.constraint->u.text, &text);
        sema_asm_constraint(text, &c);
        sema_expr(&op->u.asmop.expr);
        if (!c.output) {
            diag_error(op->loc, "asm output constraint '%s' must begin with '=' or '+'", text);
        } else if (c.unsupported) {
            diag_error(op->loc, "asm constraint '%s': '%c' is not supported", text, c.unsupported);
        } else if (!c.reg && !c.mem) {
            diag_error(op->loc, "asm output constraint '%s' allows neither a register nor memory", text);
        } else if (!poisoned(op->u.asmop.expr)) {
            check_modifiable(op->u.asmop.expr, "asm output to");
        }
        free(text);
    }
    for (i = 0; ins && i < ins->len; i++) {
        AST *op = ins->items[i];
        AsmConstraint c;
        char *text = NULL;
        long v;
        sema_string_bytes(op->u.asmop.constraint->u.text, &text);
        sema_asm_constraint(text, &c);
        if (c.mem && !c.reg && !c.imm && c.match < 0 && !c.match_name) {
            sema_expr(&op->u.asmop.expr);   /* the object itself */
        } else {
            sema_rvalue(&op->u.asmop.expr);
        }
        if (c.output) {
            diag_error(op->loc, "asm input constraint '%s' cannot have '=' or '+'", text);
        } else if (c.unsupported) {
            diag_error(op->loc, "asm constraint '%s': '%c' is not supported", text, c.unsupported);
        } else if (c.match >= 0 && (size_t)c.match >= nouts) {
            diag_error(op->loc, "asm input constraint '%s' refers to output %d, which does not exist", text, c.match);
        } else if (c.imm && !c.reg && !c.mem && c.match < 0
                   && !(type_is_integer(op->u.asmop.expr->type) && sema_const_eval(op->u.asmop.expr, &v))
                   && !is_address_constant(op->u.asmop.expr)) {
            diag_error(op->loc, "asm operand with constraint '%s' must be a constant", text);
        }
        free(text);
    }
    for (i = 0; s->u.asmst.labels && i < s->u.asmst.labels->len; i++) {
        cur_gotos = ast_list_push(cur_gotos, s->u.asmst.labels->items[i]);
    }
}

/* `&&a - &&b` (under casts): GNU label arithmetic a static initializer may hold, as an offset in the code */
int sema_label_diff(const AST *e, Symbol **a, Symbol **b) {
    const AST *l, *r;
    while (e->kind == N_IMPLICIT_CAST || e->kind == N_CAST) {
        e = e->u.cast.expr;
    }
    if (e->kind != N_BINARY || e->u.binary.op != OP_SUB) {
        return 0;
    }
    l = e->u.binary.left;
    r = e->u.binary.right;
    while (l->kind == N_IMPLICIT_CAST || l->kind == N_CAST) {
        l = l->u.cast.expr;
    }
    while (r->kind == N_IMPLICIT_CAST || r->kind == N_CAST) {
        r = r->u.cast.expr;
    }
    if (l->kind != N_LABEL_ADDR || r->kind != N_LABEL_ADDR) {
        return 0;
    }
    if (a && (!l->sym || !r->sym)) {
        return 0;   /* (checking a static initializer, the labels need not be resolved yet) */
    }
    if (a) {
        *a = l->sym;
        *b = r->sym;
    }
    return 1;
}

/* may `e` initialise an object with static storage duration? */
static int is_constant_initializer(AST *e) {
    long v;
    long double d;
    if (!e->type) {
        return 1;   /* already diagnosed */
    }
    if (type_is_vector(e->type)) {
        while (e->kind == N_IMPLICIT_CAST || e->kind == N_CAST) {
            e = e->u.cast.expr;
        }
        return e->kind == N_COMPOUND_LIT && e->sym;     /* a static compound literal's initializer is constant */
    }
    if (type_is_decimal(e->type)) {
        /* a decimal literal (negated, perhaps), or an integer or floating constant converted */
        while (e->kind == N_IMPLICIT_CAST || e->kind == N_CAST
               || (e->kind == N_UNARY && (e->u.unary.op == OP_NEG || e->u.unary.op == OP_POS))) {
            e = e->kind == N_UNARY ? e->u.unary.operand : e->u.cast.expr;
        }
        return (e->kind == N_FLOAT && type_is_decimal(e->type)) || (type_is_integer(e->type) && sema_const_eval(e, &v))
            || (type_is_floating(e->type) && float_eval(e, &d));
    }
    if (type_is_integer(e->type)) {
        return sema_const_eval(e, &v) || sema_label_diff(e, NULL, NULL);
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

/* look for aligned(n) and packed in an attribute's text; aligned(expr) is an Alignas node of its own (alignas_value) */
static void scan_attribute(const char *text, int *packed, size_t *aligned) {
    const char *p;
    if (strstr(text, "packed")) {
        *packed = 1;
    }
    p = strstr(text, "aligned");
    if (p) {
        p += strlen("aligned");
        while (*p == '_' || *p == ' ' || *p == '\t' || *p == '\n') {   /* __aligned__ (8) */
            p++;
        }
        if (*p == '(') {
            char *end;
            long n;
            p++;
            while (*p == ' ' || *p == '(') {
                p++;
            }
            n = strtol(p, &end, 0);
            while (*end == 'u' || *end == 'U' || *end == 'l' || *end == 'L') {
                end++;
            }
            while (*end == ' ') {
                end++;
            }
            if (*end == ')' && n > 0 && (size_t)n > *aligned) {
                *aligned = (size_t)n;
            }
        } else if (*aligned < 16) {
            *aligned = 16;  /* bare aligned: the largest alignment on either target */
        }
    }
}

/* the type a typeof node names: its type name's, or its expression's (which is not evaluated) */
static Type *typeof_type(AST *n) {
    if (!n->type) {     /* worked out once, though a declaration's specifiers may be read again */
        if (n->u.cast.align) {
            /* C11 _Atomic(type name) */
            Type *t = sema_type_name(n->u.cast.type);
            if (t->kind == TY_ARRAY || t->kind == TY_FUNC || t->quals) {
                diag_error(n->loc, "_Atomic cannot be applied to %s", ts(t));
            }
            n->type = type_qualify(t, TQ_ATOMIC);
        } else if (n->u.cast.type) {
            n->type = sema_type_name(n->u.cast.type);
        } else {
            n->type = sema_expr(&n->u.cast.expr);
            if (sema_is_bitfield(n->u.cast.expr)) {
                diag_error(n->loc, "'typeof' applied to a bit-field");
            }
        }
    }
    return n->type;
}

/* the alignment an Alignas node asks for (_Alignas, or an aligned attribute's expression); 0 for none or an error */
static size_t alignas_value(AST *a) {
    long v;
    if (a->u.cast.type) {
        return type_align(sema_type_name(a->u.cast.type));
    }
    if (!a->u.cast.expr->type) {    /* analysed once, though a declaration's specifiers may be read again */
        sema_rvalue(&a->u.cast.expr);
    }
    if (!type_is_integer(a->u.cast.expr->type) || !sema_const_eval(a->u.cast.expr, &v)) {
        diag_error(a->loc, "requested alignment is not an integer constant");
        return 0;
    }
    if (v < 0 || (v & (v - 1)) != 0 || (v == 0 && a->u.cast.align)) {
        diag_error(a->loc, "requested alignment %ld is not a positive power of 2", v);
        return 0;
    }
    return (size_t)v;
}

/* raise *aligned to what the Alignas node `a` asks for (not a vector_size one's) */
static void scan_alignas(AST *a, size_t *aligned) {
    size_t n;
    if (a->u.cast.align == 2) {
        return;
    }
    n = alignas_value(a);
    if (n > *aligned) {
        *aligned = n;
    }
}

/* the type __attribute__((mode(m))) makes of the integer or floating type t (keeping its signedness) */
static Type *apply_mode(Type *t, const char *m, Loc loc) {
    int u = type_is_unsigned(t);
    char name[16];
    size_t n = 0;
    while (*m == '_') {
        m++;
    }
    while ((isalnum((unsigned char)*m)) && n + 1 < sizeof(name)) {
        name[n++] = *m++;
    }
    name[n] = '\0';
    if (strcmp(name, "QI") == 0 || strcmp(name, "byte") == 0) return type_basic(u ? TY_UCHAR : TY_SCHAR);
    if (strcmp(name, "HI") == 0) return type_basic(u ? TY_USHORT : TY_SHORT);
    if (strcmp(name, "SI") == 0) return type_basic(u ? TY_UINT : TY_INT);
    if (strcmp(name, "DI") == 0 || strcmp(name, "word") == 0 || strcmp(name, "pointer") == 0) return type_basic(u ? TY_ULONG : TY_LONG);
    if (strcmp(name, "TI") == 0) return type_basic(u ? TY_UINT128 : TY_INT128);
    if (strcmp(name, "SF") == 0) return type_basic(TY_FLOAT);
    if (strcmp(name, "DF") == 0) return type_basic(TY_DOUBLE);
    if (strcmp(name, "XF") == 0) return type_basic(TY_LDOUBLE);
    if (strcmp(name, "TF") == 0) return type_basic(TY_FLOAT128);
    diag_error(loc, "unknown machine mode '%s'", name);
    return t;
}

/* the GNU type attributes mode(m) and vector_size(n) in `attrs`, applied to the base type t of a declaration */
static Type *mode_and_vector(Type *t, const ASTList *attrs, Loc loc) {
    size_t i;
    for (i = 0; attrs && i < attrs->len; i++) {
        AST *a = attrs->items[i];
        const char *m;
        if (a->kind == N_ATTRIBUTE && strncmp(a->u.text, "__attribute", 11) == 0 && (m = find_attr(a->u.text, "mode")) != NULL) {
            while (*m == ' ' || *m == '(') {
                m++;
            }
            t = type_qualify(apply_mode(t, m, a->loc), t->quals);
        }
    }
    for (i = 0; attrs && i < attrs->len; i++) {
        AST *a = attrs->items[i];
        if (a->kind == N_ALIGNAS && a->u.cast.align == 2) {
            size_t bytes = alignas_value(a);
            Type *v = bytes ? type_vector(t, bytes) : NULL;
            if (!v) {
                diag_error(a->loc, "invalid vector type: %lu bytes of %s", (unsigned long)bytes, ts(t));
            } else {
                t = type_qualify(v, t->quals);
            }
        }
    }
    (void)loc;
    return t;
}

/* the alignment __attribute__((aligned)) asks for in a declarator's attributes or its
   declaration's specifiers (`int __attribute__((aligned(8))) x`); 0 when none does */
static size_t decl_aligned(AST *d, const Specs *sp) {
    size_t aligned = sp->aligned;
    int packed = 0;
    size_t i;
    for (; d; d = d->kind == N_DECL_NAME ? NULL : d->u.declarator.inner) {
        if (!d->u.declarator.attrs) {
            continue;
        }
        for (i = 0; i < d->u.declarator.attrs->len; i++) {
            AST *a = d->u.declarator.attrs->items[i];
            if (a->kind == N_ATTRIBUTE) {
                scan_attribute(a->u.text, &packed, &aligned);
            } else if (a->kind == N_ALIGNAS) {
                scan_alignas(a, &aligned);
            }
        }
    }
    return aligned;
}

/* the type a declaration's aligned attribute gives what it declares: a typedef takes the
   alignment as asked (gcc lets it lower one too), a member or an object only ever gains */
static Type *apply_aligned(Type *t, size_t aligned, int is_typedef) {
    if (!aligned || (aligned & (aligned - 1)) != 0) {
        return t;
    }
    if (is_typedef || aligned > type_align(t)) {
        return type_with_align(t, aligned);
    }
    return t;
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

        if (static_assertion(decl)) {
            continue;
        }
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
            if (!sd->u.init_decl.init) {
                mt = apply_aligned(mt, decl_aligned(declarator, &sp), 0);   /* not a bit-field */
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
            if (type_is_vm(mt) && (bits >= 0 || !cur_return)) {
                diag_error(sd->loc, "field '%s' has a variably modified type", name ? name : "");
                mt = int_type();
            } else if (type_is_vla(mt)) {
                r->vm = 1;      /* GNU, in a function: laid out when the definition is reached */
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
    {
        int sso = attrs->sso;
        if (spec->u.record.attrs) {
            for (i = 0; i < spec->u.record.attrs->len; i++) {
                AST *a = spec->u.record.attrs->items[i];
                if (a->kind == N_ALIGNAS) {
                    scan_alignas(a, &r->aligned);
                } else {
                    scan_attribute(a->u.text, &r->packed, &r->aligned);
                    if (find_attr(a->u.text, "scalar_storage_order")) {
                        sso = strstr(a->u.text, "big") ? 1 : 2;
                    }
                }
            }
        }
        record_layout(r);
        /* both targets are little-endian: big-endian is the reversed order, for each scalar member */
        for (i = 0; sso == 1 && i < r->nmembers; i++) {
            const Type *mt = r->members[i].type;
            while (mt->kind == TY_ARRAY) {
                mt = mt->base;
            }
            r->members[i].reverse = type_is_scalar(mt) && !type_is_complex(mt);
        }
    }
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
    int n_va_list = 0, n_int128 = 0, n_uint128 = 0, n_bool = 0, n_float16 = 0, n_float128 = 0, n_complex = 0;
    int n_decimal = 0, n_other_decimal = 0;
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
                if (s->u.storage == STORAGE_THREAD) {
                    if (!allow_storage) {
                        diag_error(s->loc, "storage class specifier is not allowed here");
                    } else if (out->is_thread) {
                        diag_error(s->loc, "duplicate _Thread_local");
                    }
                    out->is_thread = 1;
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
                } else if (s->u.qual == QUAL_ATOMIC) {
                    quals |= TQ_ATOMIC;
                }
                break;

            case N_ATTRIBUTE:
                scan_attribute(s->u.text, &out->packed, &out->aligned);
                if (find_attr(s->u.text, "scalar_storage_order")) {
                    out->sso = strstr(s->u.text, "big") ? 1 : 2;
                }
                break;

            case N_ALIGNAS:
                scan_alignas(s, &out->aligned);
                if (!s->u.cast.align) {
                    out->has_alignas = 1;
                }
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
                    case TS_FLOAT64X: n_long++; n_double++; break;
                    case TS_FLOAT128: n_float128++; break;
                    case TS_DECIMAL32: n_decimal = 32; n_other_decimal++; break;
                    case TS_DECIMAL64: n_decimal = 64; n_other_decimal++; break;
                    case TS_DECIMAL128: n_decimal = 128; n_other_decimal++; break;
                    case TS_COMPLEX: n_complex++; break;
                    case TS_AUTO_TYPE: n_other++; other = s; break;
                }
                break;

            case N_STRUCT_SPEC:
            case N_ENUM_SPEC:
            case N_TYPEDEF_NAME:
            case N_TYPEOF:
                n_other++;
                other = s;
                break;

            default:
                break;
        }
    }

    if (n_other > 1 || (n_other && (n_void + n_char + n_short + n_int + n_long + n_float
            + n_double + n_signed + n_unsigned + n_va_list + n_int128 + n_uint128 + n_bool + n_float16 + n_float128 + n_complex
            + n_other_decimal))) {
        diag_error(loc, "invalid combination of type specifiers");
        n_other = 0;
    }
    if (n_other) {
        if (other->kind == N_TYPE_SPEC) {
            /* __auto_type: the declaration takes the initializer's type (sema_declaration) */
            t = int_type();
            out->auto_type = 1;
            out->only_tag = 0;
        } else if (other->kind == N_TYPEOF) {
            t = typeof_type(other);
            out->only_tag = 0;
        } else if (other->kind == N_TYPEDEF_NAME) {
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
    } else if (n_other_decimal) {
        t = type_basic(n_decimal == 32 ? TY_DECIMAL32 : n_decimal == 64 ? TY_DECIMAL64 : TY_DECIMAL128);
        if (n_other_decimal > 1 || n_char || n_short || n_int || n_long || n_float || n_double || n_signed || n_unsigned
                || n_float128 || n_complex) {
            diag_error(loc, "invalid combination of type specifiers");
        }
    } else if (n_float128) {
        t = type_basic(TY_FLOAT128);  /* x86_64 only: Apple's arm64 has none */
        if (n_float128 > 1 || n_char || n_short || n_int || n_long || n_float || n_double || n_signed || n_unsigned) {
            diag_error(loc, "invalid combination of type specifiers");
        }
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
        } else if (t->kind == TY_FLOAT128) {
            t = type_basic(TY_CFLOAT128);
        } else if (t->kind == TY_INT && !n_int && !n_signed && !n_unsigned) {
            t = type_basic(TY_CDOUBLE);
        } else {
            diag_error(loc, "'_Complex' requires a floating type, not %s", ts(t));
        }
    }
    if (out->has_alignas && out->is_typedef) {
        diag_error(loc, "a typedef cannot have '_Alignas'");
    }
    t = mode_and_vector(t, specs, loc);
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
            } else if (s->u.qual == QUAL_ATOMIC) {
                q |= TQ_ATOMIC;
            }
        }
    }
    return q;
}

/*
 * C99 lets a parameter's outermost array declarator carry qualifiers and
 * `static` (`int a[const static 4]`), and lets a prototype's say `[*]`.
 * array_param_top is that declarator while a parameter is analysed, and
 * star_ok is set inside a prototype that is not a definition's.
 */
static AST *array_param_top;
static int star_ok;

/* the declarator node that makes the outermost type of declarator `d`, or NULL */
static AST *declarator_top(AST *d) {
    AST *top = NULL;
    for (; d && d->kind != N_DECL_NAME; d = d->u.declarator.inner) {
        top = d;
    }
    return top;
}

/* the qualifiers `[const 5]` gives the pointer a parameter declared by `d` becomes */
static unsigned array_param_quals(AST *d) {
    AST *top = declarator_top(d);
    return top && top->kind == N_DECL_ARRAY ? quals_of(top->u.declarator.quals) : 0;
}

/* a parameter's type after the array and function adjustments */
static Type *adjust_param(Type *t) {
    if (t->kind == TY_ARRAY && !t->vector) {
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
    /* mode and vector_size after a declarator change the declaration's base type */
    for (node = d; node; node = node->kind == N_DECL_NAME ? NULL : node->u.declarator.inner) {
        t = mode_and_vector(t, node->u.declarator.attrs, node->loc);
    }
    node = d;
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
                int vla = 0;
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
                    if (poisoned(node->u.declarator.size)) {
                        n = 1;
                    } else if (!type_is_integer(node->u.declarator.size->type)) {
                        diag_error(node->loc, "size of array has non-integer type %s", ts(node->u.declarator.size->type));
                        n = 1;
                    } else if (!sema_const_eval(node->u.declarator.size, &n)) {
                        /* C99: a variable length array, sized when the declarator is reached; where one may appear is checked by the declaration */
                        vla = 1;
                        n = 0;
                    } else if (n < 0) {
                        /* a zero length is a GNU extension, common in system headers */
                        diag_error(node->loc, "array size is negative");
                        n = 1;
                    }
                    length = (long)n;
                }
                if (node->u.declarator.quals && node != array_param_top) {
                    diag_error(node->loc, "static or type qualifiers in an array declarator are only allowed in a parameter's outermost one");
                }
                if (node->u.declarator.star) {
                    size_t qi;
                    for (qi = 0; node->u.declarator.quals && qi < node->u.declarator.quals->len; qi++) {
                        if (node->u.declarator.quals->items[qi]->kind == N_STORAGE_CLASS) {
                            diag_error(node->loc, "'static' may not be used with an unspecified variable length array size");
                            break;
                        }
                    }
                    if (!star_ok) {
                        diag_error(node->loc, "'[*]' is only allowed in a function prototype");
                    } else if (node != array_param_top) {
                        /* sized by nothing: a prototype's types are compared, never laid out */
                        vla = 1;
                    }
                }
                if (!vla && node->u.declarator.size) {
                    /* `int a[2][n]`: an array of a variable length one is sized at run time too */
                    Type *e;
                    for (e = t; e->kind == TY_ARRAY && !e->vla; e = e->base) {
                    }
                    vla = type_is_vla(e);
                }
                t = vla ? type_vla(t, node->u.declarator.size) : type_array(t, length);
                node->type = t;
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
                } else if (t->kind == TY_ARRAY && !t->vector) {
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
                            AST *saved_top = array_param_top;
                            int saved_star = star_ok;
                            array_param_top = declarator_top(p->u.typed.declarator);
                            star_ok = node != defining_fn;
                            pt = sema_declarator(p->u.typed.declarator, sp.type, &pname);
                            array_param_top = saved_top;
                            star_ok = saved_star;
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
                        if (pt->kind == TY_POINTER && p->u.typed.declarator) {
                            pt = type_qualify(pt, array_param_quals(p->u.typed.declarator));
                        }
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
    if (type_is_vector(to) || type_is_vector(from)) {
        /* a vector from one of the same size (GCC's lax vector conversions: the bits as they are) */
        if (!type_is_vector(to) || !type_is_vector(from) || type_size(to) != type_size(from)) {
            diag_error(loc, "%s %s from incompatible type %s", what, ts(to), ts(from));
        } else if (!type_compatible_unqual(to, from)) {
            ast_implicit_cast(slot, to);
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
 * every value it can take, else to unsigned int. A field wider than an int
 * (an extension) keeps its declared type, as clang has it; gcc instead
 * gives it a type of its own width (`unsigned long long:40`), so that
 * arithmetic on it wraps at that width (Type.bits), and jcc does too where
 * gcc is the platform's compiler. The member keeps its declared type for
 * assignment; this is for operands.
 *
 * The value of an assignment, a compound assignment or a prefix ++ or -- to
 * a bit-field, and of a comma expression ending in one, has the bit-field's
 * type and promotes the same way, as gcc and clang have it. Postfix ++ and
 * -- are where they differ: gcc promotes by the width, clang keeps the
 * declared type.
 */
static int bitfield_value_bits(const AST *e) {
    for (;;) {
        if (sema_is_bitfield(e)) {
            return e->u.member.member->bits;
        }
        if (e->kind == N_ASSIGN) {
            e = e->u.binary.left;
        } else if (e->kind == N_BINARY && e->u.binary.op == OP_COMMA) {
            e = e->u.binary.right;
        } else if (e->kind == N_UNARY
                   && (e->u.unary.op == OP_PREINC || e->u.unary.op == OP_PREDEC
                       || (target->gcc && (e->u.unary.op == OP_POSTINC || e->u.unary.op == OP_POSTDEC)))) {
            e = e->u.unary.operand;
        } else {
            return -1;
        }
    }
}

static Type *promote_bitfield(AST **slot) {
    AST *e = *slot;
    size_t int_bits;
    size_t bits;
    int b = bitfield_value_bits(e);
    Type *p;

    if (b < 0 || !type_is_integer(e->type)) {
        return e->type;
    }
    int_bits = type_size(int_type()) * 8;
    bits = (size_t)b;
    if (bits < int_bits || (bits == int_bits && !type_is_unsigned(e->type))) {
        p = int_type();
    } else if (bits == int_bits) {
        p = type_basic(TY_UINT);
    } else if (target->gcc && bits < type_size(e->type) * 8) {
        p = type_bitfield(type_unqualified(e->type), (int)bits);
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
    if (e->type->kind == TY_ARRAY && !e->type->vector) {
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
            if (fn_depth > 1 && sym->scope->kind != SCOPE_FILE && sym->storage != STORAGE_STATIC && sym->storage != STORAGE_EXTERN) {
                /* a local of an enclosing function, which a nested one uses: reached through the static chain */
                Scope *s = sym->scope;
                int k;
                while (s && s->kind != SCOPE_FUNCTION) {
                    s = s->parent;
                }
                for (k = fn_depth - 2; k >= 0 && s != fn_scope_stack[k]; k--) {
                }
                if (s != fn_scope_stack[fn_depth - 1] && k >= 0) {
                    sym->captured = 1;
                    sym->owner_fn = fn_stack[k];
                }
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
            if (type_is_vector(t)) {
                e->type = t;
                e->lvalue = 0;
                return e->type;
            }
            if (!type_is_arithmetic(t)) {
                diag_error(e->loc, "invalid operand of type %s to unary '%s'", ts(t), op_name(e->u.unary.op));
                return error_type(e);
            }
            e->type = type_is_integer(t) ? promote_operand(operand) : t;
            e->lvalue = 0;
            return e->type;

        case OP_BITNOT:
            t = sema_rvalue(operand);
            if (type_is_vector(t) && type_is_integer(t->base)) {
                e->type = t;
                e->lvalue = 0;
                return e->type;
            }
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

/*
 * __builtin_shuffle(a, mask) and (a, b, mask): a vector of a's elements,
 * as many as the integer vector mask has, each the one of a (or of a
 * followed by b) that mask's element picks, modulo their number.
 */
static Type *sema_shuffle(AST *e) {
    AST **f = &e->u.call.func;
    ASTList *args = e->u.call.args;
    Symbol *sym;
    Type *a, *m;
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
    if (args->len != 2 && args->len != 3) {
        diag_error(e->loc, "'__builtin_shuffle' takes two or three arguments");
        return error_type(e);
    }
    a = args->items[0]->type;
    m = args->items[args->len - 1]->type;
    if (!type_is_vector(a) || !type_is_vector(m) || !type_is_integer(m->base)
            || (args->len == 3 && !type_compatible_unqual(a, args->items[1]->type))) {
        diag_error(e->loc, "'__builtin_shuffle' takes vectors of one type and an integer vector mask");
        return error_type(e);
    }
    e->type = type_vector(a->base, (size_t)m->length * type_size(a->base));
    if (!e->type) {
        diag_error(e->loc, "'__builtin_shuffle': no vector of %ld %s", m->length, ts(a->base));
        return error_type(e);
    }
    return e->type;
}

/* the signed integer type of `size` bytes: a vector comparison's elements */
static Type *signed_of_size(size_t size) {
    return type_basic(size == 1 ? TY_SCHAR : size == 2 ? TY_SHORT : size == 4 ? TY_INT : size == 8 ? TY_LLONG : TY_INT128);
}

/*
 * GNU vector arithmetic: element by element on two vectors of one type,
 * or a vector and a scalar (converted to the element type, and used for
 * every element). A comparison gives a vector of signed integers of the
 * elements' size, each -1 (true) or 0.
 */
static Type *sema_vector_binary(AST *e, OpKind op, AST **l, AST **r, Type *lt, Type *rt) {
    Type *v = type_is_vector(lt) ? lt : rt;
    Type *elem = v->base;
    int shift = op == OP_SHL || op == OP_SHR;
    if (type_is_vector(lt) && type_is_vector(rt)) {
        if (type_size(lt) != type_size(rt) || type_size(lt->base) != type_size(rt->base)
                || type_is_floating(lt->base) != type_is_floating(rt->base)) {
            diag_error(e->loc, "invalid operands to binary %s (%s and %s)", op_name(op), ts(lt), ts(rt));
            return error_type(e);
        }
        if (!shift && !type_compatible_unqual(lt, rt)) {
            ast_implicit_cast(r, lt);
        }
    } else {
        AST **s = type_is_vector(lt) ? r : l;
        Type *st = type_is_vector(lt) ? rt : lt;
        if (!type_is_arithmetic(st) || type_is_complex(st)) {
            diag_error(e->loc, "invalid operands to binary %s (%s and %s)", op_name(op), ts(lt), ts(rt));
            return error_type(e);
        }
        coerce(s, elem);
    }
    switch (op) {
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV:
            e->type = type_unqualified(v);
            return e->type;
        case OP_MOD: case OP_BITAND: case OP_BITOR: case OP_BITXOR: case OP_SHL: case OP_SHR:
            if (!type_is_integer(elem)) {
                diag_error(e->loc, "invalid operands to binary %s (%s and %s)", op_name(op), ts(lt), ts(rt));
                return error_type(e);
            }
            e->type = type_unqualified(v);
            return e->type;
        case OP_EQ: case OP_NE: case OP_LT: case OP_LE: case OP_GT: case OP_GE:
            e->type = type_vector(signed_of_size(type_size(elem)), type_size(v));
            return e->type;
        default:
            diag_error(e->loc, "invalid operands to binary %s (%s and %s)", op_name(op), ts(lt), ts(rt));
            return error_type(e);
    }
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
    if (bitfield_value_bits(*l) >= 0) {
        lt = promote_bitfield(l);
    }
    if (bitfield_value_bits(*r) >= 0) {
        rt = promote_bitfield(r);
    }
    if (type_is_vector(lt) || type_is_vector(rt)) {
        return sema_vector_binary(e, op, l, r, lt, rt);
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
    if (type_is_vector(lt)) {
        /* v op= x: as v = v op x, the result a vector of v's type */
        if (type_is_vector(rt) ? type_size(rt) != type_size(lt) : !type_is_arithmetic(rt)) {
            diag_error(e->loc, "invalid operands to '%s' (%s and %s)", op_name(op), ts(lt), ts(rt));
            return error_type(e);
        }
        if (!type_is_vector(rt)) {
            coerce(r, lt->base);
        } else if (!type_compatible_unqual(lt, rt) && op != OP_SHL_ASSIGN && op != OP_SHR_ASSIGN) {
            ast_implicit_cast(r, result);
        }
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
        /* the usual arithmetic conversions, which promote a bit-field by its width */
        if (bitfield_value_bits(*a) >= 0) {
            at = promote_bitfield(a);
        }
        if (bitfield_value_bits(*b) >= 0) {
            bt = promote_bitfield(b);
        }
        Type *common = type_arith_common(at, bt);
        coerce(a, common);
        coerce(b, common);
        e->type = common;
        return common;
    }
    if ((type_is_record(at) || type_is_vector(at)) && type_compatible_unqual(at, bt)) {
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
    int type_generic;
    AST *callee;

    e->lvalue = 0;
    if ((*f)->kind == N_IDENT && !(*f)->sym) {
        const AtomicBuiltin *ab = sema_atomic_builtin((*f)->u.name);
        if (ab) {
            return sema_atomic(e, ab);
        }
        if (strcmp((*f)->u.name, "__builtin_shuffle") == 0) {
            return sema_shuffle(e);
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
    /* a type generic builtin (the "i(.)" ones): its operands keep their own types, a float too */
    callee = *f;
    while (callee->kind == N_IMPLICIT_CAST) {
        callee = callee->u.cast.expr;
    }
    type_generic = callee->kind == N_IDENT && callee->sym && !callee->sym->decl && strncmp(callee->u.name, "__builtin_", 10) == 0
        && fn->has_proto && fn->nparams == 0 && fn->variadic;
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
            } else if (!type_generic) {
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

    if (type_is_vector(at)) {
        /* an element of a vector: an lvalue when the vector is one */
        if (!type_is_integer(it)) {
            diag_error(e->loc, "vector subscript of type %s is not an integer", ts(it));
            return error_type(e);
        }
        e->type = at->base;
        e->lvalue = (*a)->lvalue > 0;
        return e->type;
    }
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

/* an analysed cast of a struct or union to its own type */
static int record_cast(const AST *e) {
    return e->type && type_is_record(e->type) && e->u.cast.expr->type && type_is_record(e->u.cast.expr->type);
}

static Type *sema_cast(AST *e) {
    Type *to = sema_type_name(e->u.cast.type);
    Type *from = sema_rvalue(&e->u.cast.expr);

    e->lvalue = 0;
    e->type = type_unqualified(to);
    if (to->kind == TY_VOID) {
        return e->type;
    }
    if (type_is_record(to) && type_compatible_unqual(from, to)) {
        return e->type;     /* record_cast: sema_expr drops the cast */
    }
    if (type_is_vector(to) || type_is_vector(from)) {
        /* between vectors, or a vector and an integer, of one size: the bits as they are */
        if (type_size(to) != type_size(from) || !(type_is_vector(to) || type_is_integer(to) || to->kind == TY_POINTER)
                || !(type_is_vector(from) || type_is_integer(from) || from->kind == TY_POINTER)) {
            diag_error(e->loc, "cannot cast %s to %s", ts(from), ts(to));
            return error_type(e);
        }
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
    const char *op = e->u.cast.align ? "_Alignof" : "sizeof";

    e->lvalue = 0;
    e->type = type_size_t();
    if (e->kind == N_SIZEOF_TYPE) {
        t = sema_type_name(e->u.cast.type);
    } else {
        t = sema_expr(&e->u.cast.expr);
        if (sema_is_bitfield(e->u.cast.expr)) {
            diag_error(e->loc, "invalid application of '%s' to a bit-field", op);
            return e->type;
        }
    }
    if (t->kind == TY_FUNC) {
        /* a GNU extension: the size is 1 (and the alignment the function's) */
        if (!e->u.cast.align) {
            diag_warning(e->loc, "invalid application of '%s' to a function type", op);
        }
    } else if (!type_is_complete(t) && t->kind != TY_VOID) {
        diag_error(e->loc, "invalid application of '%s' to incomplete type %s", op, ts(t));
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

/*
 * `(T){ ... }`: an unnamed object of type T (an array of unknown size
 * takes its size from the list). At file scope, or inside the
 * initializer of a static object, it has static storage: a file scope
 * symbol with a name no C program can spell, emitted as data like any
 * static. Otherwise it is automatic, and the code that evaluates it
 * initializes a frame slot of its own (src/lower.c).
 */
static Type *sema_compound_lit(AST *e) {
    Type *t = sema_type_name(e->u.cast.type);
    AST *holder = e->u.cast.expr;
    int is_static = init_static_ctx || !cur_return;
    int saved = init_static_ctx;

    e->lvalue = 1;
    if (t->kind == TY_FUNC || t->kind == TY_VOID
            || (!type_is_complete(t) && !(t->kind == TY_ARRAY && t->length < 0 && type_is_complete(t->base)))) {
        diag_error(e->loc, "compound literal of incomplete type %s", ts(t));
        return error_type(e);
    }
    if (type_is_vm(t)) {
        diag_error(e->loc, "compound literal has variable length array type %s", ts(t));
        return error_type(e);
    }
    init_static_ctx = is_static;
    sema_initializer(&holder->u.init_decl.init, &t, is_static);
    init_static_ctx = saved;
    holder->type = t;
    e->type = t;
    if (is_static) {
        char name[48];
        Scope *here = cur_symtab->current;
        Symbol *sym;
        snprintf(name, sizeof(name), "__compound_literal.%d", ++ncompound_lits);
        cur_symtab->current = cur_symtab->file;
        sym = symtab_declare(cur_symtab, NS_ORDINARY, SYM_VARIABLE, name, holder, e->loc);
        cur_symtab->current = here;
        sym->type = t;
        sym->storage = STORAGE_STATIC;
        sym->def = holder;
        holder->sym = sym;
        e->sym = sym;
    }
    return t;
}

/*
 * C11 `_Generic(e, T1: e1, ..., default: d)`: the controlling expression
 * is not evaluated; its type after lvalue conversion picks the
 * association with a compatible type (or the default), and that
 * expression replaces the whole node.
 */
static Type *sema_generic(AST **slot) {
    AST *e = *slot;
    ASTList *assocs = e->u.call.args;
    Type *ct = sema_rvalue(&e->u.call.func);
    AST **chosen = NULL;
    AST **deflt = NULL;
    AST *r;
    size_t i;

    for (i = 0; i < assocs->len; i++) {
        AST *a = assocs->items[i];
        Type *at;
        if (!a->u.cast.type) {
            if (deflt) {
                diag_error(a->loc, "duplicate default association in _Generic");
            }
            deflt = &a->u.cast.expr;
            continue;
        }
        at = sema_type_name(a->u.cast.type);
        if (!type_is_complete(at) || at->kind == TY_FUNC) {
            diag_error(a->loc, "type %s in a _Generic association is not a complete object type", ts(at));
            continue;
        }
        if (!poisoned(e->u.call.func) && type_compatible(ct, at)) {
            if (chosen) {
                diag_error(a->loc, "controlling expression type %s is compatible with more than one association", ts(ct));
            } else {
                chosen = &a->u.cast.expr;
            }
        }
    }
    if (!chosen) {
        chosen = deflt;
    }
    if (!chosen) {
        if (!poisoned(e->u.call.func)) {
            diag_error(e->loc, "controlling expression type %s is not compatible with any _Generic association", ts(ct));
        }
        return error_type(e);
    }
    r = *chosen;
    *chosen = NULL;
    *slot = r;
    ast_free(e);
    return sema_expr(slot);
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
            if (sema_cast(e) && record_cast(e)) {
                /* GNU: a cast of a struct or union to its own type is the operand itself, as a value */
                *slot = e->u.cast.expr;
                e->u.cast.expr = NULL;
                ast_free(e);
                (*slot)->lvalue = 0;
            }
            return (*slot)->type;

        case N_COMPOUND_LIT:
            return sema_compound_lit(e);

        case N_TYPES_COMPAT: {
            /* GNU: 1 when the two types are compatible, their top-level qualifiers aside; an integer constant */
            Type *a = type_unqualified(sema_type_name(e->u.cast.type));
            Type *b = type_unqualified(sema_type_name(e->u.cast.expr));
            int same = type_compatible(a, b);
            ast_free(e->u.cast.type);
            ast_free(e->u.cast.expr);
            e->kind = N_INT;
            e->u.text = strdup(same ? "1" : "0");
            return sema_expr(slot);
        }

        case N_GENERIC:
            return sema_generic(slot);

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
            int dynamic = 0;    /* a run time index, or sizes: lowering works it out */
            for (k = 0; k < e->u.desig.path->len; k++) {
                AST *d = e->u.desig.path->items[k];
                if (type_is_record(t) && t->record->vm) {
                    dynamic = 1;
                }
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
                    if (!type_is_integer(d->u.cast.expr->type)) {
                        diag_error(d->loc, "offsetof index is not an integer");
                        return error_type(e);
                    }
                    if (!sema_const_eval(d->u.cast.expr, &idx) || type_is_vla(t->base) || type_is_vla(t)) {
                        dynamic = 1;    /* GNU: as &((T *)0)->path at run time */
                        idx = 0;
                    }
                    off += (unsigned long)idx * type_size(t->base);
                    t = t->base;
                }
                if (type_is_record(t) && t->record->vm) {
                    dynamic = 1;
                }
            }
            if (dynamic) {
                e->type = type_size_t();
                e->lvalue = 0;
                return e->type;
            }
            snprintf(text, sizeof(text), "%luUL", off);
            *slot = ast_text(N_INT, e->loc, xstrdup(text));
            ast_free(e);
            return sema_expr(slot);
        }

        case N_LABEL_ADDR:
            /* GNU &&label: resolved with the gotos at the end of the function */
            e->type = type_pointer(type_basic(TY_VOID));
            e->lvalue = 0;
            if (!cur_return) {
                diag_error(e->loc, "label address outside a function");
            } else {
                cur_gotos = ast_list_push(cur_gotos, e);
            }
            return e->type;

        case N_STMT_EXPR: {
            ASTList *items = e->u.stmt.body->u.block.items;
            AST *last = items->len ? items->items[items->len - 1] : NULL;
            sema_stmt(e->u.stmt.body);
            e->lvalue = 0;
            while (last && last->kind == N_LABEL) {
                last = last->u.stmt.body;   /* `({ ...; done: v; })`: a labeled last statement still gives the value */
            }
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

    if ((t->kind == TY_ARRAY && !t->vector) || t->kind == TY_FUNC || (t->kind == TY_VA_LIST && arch_info->va_list_is_array)) {
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
 * once all of them are. A `.name` naming a member of an anonymous struct
 * or union selects that anonymous member without being applied; `anon`
 * says so, and the designator then applies inside it.
 */
typedef struct Cursor {
    ASTList *list;
    size_t pos;
    size_t path_pos;
    int anon;
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
    return c->list->items[c->pos]->kind == N_DESIGNATED && c->path_pos == 0 && !c->anon;
}

static void cur_advance(Cursor *c) {
    c->pos++;
    c->path_pos = 0;
    c->anon = 0;
}

/* apply the designator at the cursor: it selected the member it names, or (`through_anon`) the anonymous one holding it */
static void cur_apply(Cursor *c, int through_anon) {
    if (through_anon) {
        c->anon = 1;
    } else {
        c->path_pos++;
        c->anon = 0;
    }
}

/* `top`: the list belongs to this object's own braces, so designators address it */
static void init_object(Type *t, Cursor *c, int is_static, long *count_out, int top);

/* one index of an array designator; -1 if it is not usable */
static long designator_bound(AST *d, AST **slot, Type *array) {
    long v;
    sema_rvalue(slot);
    if (!type_is_integer((*slot)->type) || !sema_const_eval(*slot, &v)) {
        diag_error(d->loc, "array designator is not an integer constant expression");
        return -1;
    }
    if (v < 0 || (array->length >= 0 && v >= array->length)) {
        diag_error(d->loc, "array designator index %ld is out of range", v);
        return -1;
    }
    return (long)v;
}

/* resolve `[expr]` to an index, and `*last` to the end of a GNU range `[expr ... last]` (expr itself otherwise); -1 if it is not usable */
static long designated_index(AST *d, Type *array, long *last) {
    long first;
    if (d->kind != N_DESIG_INDEX) {
        diag_error(d->loc, "field designator cannot initialize an array");
        return -1;
    }
    first = designator_bound(d, &d->u.cast.expr, array);
    *last = first;
    if (first >= 0 && d->u.cast.type) {
        *last = designator_bound(d, &d->u.cast.type, array);
        if (*last < 0) {
            return -1;
        }
        if (*last < first) {
            diag_error(d->loc, "empty index range in initializer");
            return -1;
        }
    }
    return first;
}

/* resolve `.name` to a member index (see record_designator_index); -1 if there is none */
static long designated_member(AST *d, Record *r, int *through_anon) {
    long k;
    *through_anon = 0;
    if (d->kind != N_DESIG_MEMBER) {
        diag_error(d->loc, "array designator cannot initialize %s", type_kind_name(r->kind));
        return -1;
    }
    k = record_designator_index(r, d->u.name, through_anon);
    if (k >= 0) {
        return k;
    }
    diag_error(d->loc, "%s %s has no member named '%s'", type_kind_name(r->kind), r->tag ? r->tag : "", d->u.name);
    return -1;
}

static void init_scalar(AST **slot, Type *t, int is_static) {
    AST *init = *slot;
    if (init->kind == N_INIT_LIST) {
        if (init->u.list->len == 0) {
            return;     /* `{}`: zero */
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
    c.anon = 0;
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
        if ((type_is_record(t) || type_is_vector(t)) && (*slot)->kind != N_STRING) {
            /* a struct expression initialises the whole member (a string literal cannot be one: it starts an elided char array) */
            Type *et = sema_rvalue(slot);
            if (type_is_vector(t) && type_is_vector(et) && type_size(et) == type_size(t)) {
                convert_assign(slot, t, "initializing");
                cur_advance(c);
                if (is_static && !is_constant_initializer(*slot)) {
                    diag_error((*slot)->loc, "initializer element is not a compile-time constant");
                }
                return;
            }
            if (type_is_record(et) && type_compatible_unqual(et, t)) {
                cur_advance(c);
                if (is_static && !((*slot)->kind == N_COMPOUND_LIT && (*slot)->sym)) {
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
                long last;
                long k = designated_index(d, t, &last);
                if (k < 0) {
                    cur_advance(c);
                    continue;
                }
                n = k;
                cur_apply(c, 0);
                if (last > k) {
                    /* a range: each element takes the same initializer, so checking it once checks them all */
                    n = last;
                }
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
                int through_anon;
                long k = designated_member(d, r, &through_anon);
                if (k < 0) {
                    cur_advance(c);
                    continue;
                }
                i = (size_t)k;
                cur_apply(c, through_anon);
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
        /* one member, unless designators (which may follow one another in the union's own braces) name more */
        Record *r = t->record;
        int done = 0;
        while (r->nmembers && c->pos < c->list->len) {
            AST *d;
            size_t i = 0;
            if (!top && cur_is_fresh_designation(c)) {
                break;
            }
            d = cur_designator(c);
            if (d) {
                int through_anon;
                long k = designated_member(d, r, &through_anon);
                if (k < 0) {
                    cur_advance(c);
                    continue;
                }
                i = (size_t)k;
                cur_apply(c, through_anon);
            } else if (done) {
                break;
            }
            init_elem(r->members[i].type, c, is_static);
            done = 1;
        }
        return;
    }
    if (c->pos < c->list->len) {
        init_elem(t, c, is_static);
    }
}

static void sema_initializer_in(AST **slot, Type **pt, int is_static);

static void sema_initializer(AST **slot, Type **pt, int is_static) {
    int saved = init_static_ctx;
    init_static_ctx = is_static;
    sema_initializer_in(slot, pt, is_static);
    init_static_ctx = saved;
}

static void sema_initializer_in(AST **slot, Type **pt, int is_static) {
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
    if (type_is_vector(t)) {
        sema_rvalue(slot);
        convert_assign(slot, t, "initializing");
        if (is_static && !is_constant_initializer(*slot)) {
            diag_error(init->loc, "initializer element is not a compile-time constant");
        }
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
        } else if (is_static && !(init->kind == N_COMPOUND_LIT && init->sym)) {
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
        if (specs->items[i]->kind != N_ATTRIBUTE && specs->items[i]->kind != N_ALIGNAS) {
            return 0;
        }
    }
    return specs->len > 0;
}

/* a Declaration (or struct member declaration) that is a _Static_assert: check it; returns 1 if it was one */
static int static_assertion(AST *decl) {
    AST *sa;
    long v;
    if (decl->u.decl.declarators->len != 0 || decl->u.decl.specs->len != 1
            || decl->u.decl.specs->items[0]->kind != N_STATIC_ASSERT) {
        return 0;
    }
    sa = decl->u.decl.specs->items[0];
    sema_rvalue(&sa->u.binary.left);
    if (poisoned(sa->u.binary.left)) {
        return 1;
    }
    if (!type_is_integer(sa->u.binary.left->type) || !sema_const_eval(sa->u.binary.left, &v)) {
        diag_error(sa->loc, "static assertion expression is not an integer constant expression");
    } else if (v == 0) {
        if (sa->u.binary.right) {
            diag_error(sa->loc, "static assertion failed: %s", sa->u.binary.right->u.text);
        } else {
            diag_error(sa->loc, "static assertion failed");
        }
    }
    return 1;
}

/* the attribute `word` (or `__word__`) in an __attribute__'s text, as a whole word: a pointer just past it, else NULL */
static const char *find_attr(const char *text, const char *word) {
    size_t n = strlen(word);
    const char *p = text;
    while ((p = strstr(p, word)) != NULL) {
        const char *b = p;
        const char *e = p + n;
        if (b - text >= 2 && b[-1] == '_' && b[-2] == '_') {
            b -= 2;
        }
        if (e[0] == '_' && e[1] == '_') {
            e += 2;
        }
        if ((b == text || !(isalnum((unsigned char)b[-1]) || b[-1] == '_')) && !(isalnum((unsigned char)*e) || *e == '_')) {
            return e;
        }
        p += n;
    }
    return NULL;
}

/* alias("target") and weak in a list of attributes: Symbol.alias and Symbol.weak (once weak, a symbol stays so) */
static void scan_alias_list(Symbol *sym, const ASTList *attrs, Loc loc) {
    size_t i;
    for (i = 0; attrs && i < attrs->len; i++) {
        const char *text = attrs->items[i]->u.text;
        const char *p;
        const char *end;
        if (attrs->items[i]->kind != N_ATTRIBUTE || strncmp(text, "__attribute", 11) != 0) {
            continue;   /* not an attribute: a specifier, an Alignas node, or __asm("name") */
        }
        if (find_attr(text, "weak")) {
            sym->weak = 1;
        }
        p = find_attr(text, "alias");
        if (!p) {
            continue;
        }
        while (*p == ' ') {
            p++;
        }
        if (*p == '(') {
            p++;
        }
        while (*p == ' ') {
            p++;
        }
        end = *p == '"' ? strchr(p + 1, '"') : NULL;
        if (!end) {
            diag_error(loc, "the alias attribute takes a string, the name of its target");
            continue;
        }
        free(sym->alias);
        sym->alias = xmalloc((size_t)(end - p));
        memcpy(sym->alias, p + 1, (size_t)(end - p - 1));
        sym->alias[end - p - 1] = '\0';
    }
}

/* __attribute__((alias("target"))) and weak on a file scope declaration, in its specifiers or its declarator */
static void scan_alias_attrs(Symbol *sym, const ASTList *specs, AST *d, Loc loc) {
    scan_alias_list(sym, specs, loc);
    for (; d; d = d->kind == N_DECL_NAME ? NULL : d->u.declarator.inner) {
        scan_alias_list(sym, d->u.declarator.attrs, loc);
    }
}

/* an asm label, `int f(void) __asm__("g")`: the symbol's assembler name (Symbol.asm_name) */
static void scan_asm_label(Symbol *sym, AST *d) {
    size_t i;
    for (; d; d = d->kind == N_DECL_NAME ? NULL : d->u.declarator.inner) {
        for (i = 0; d->u.declarator.attrs && i < d->u.declarator.attrs->len; i++) {
            AST *a = d->u.declarator.attrs->items[i];
            char *name;
            if (a->kind != N_ATTRIBUTE || strncmp(a->u.text, "__attribute", 11) == 0 || (a->u.text[0] != 'a' && a->u.text[0] != '_')) {
                continue;
            }
            name = sema_asm_text(a->u.text, a->loc);
            if (!name) {
                continue;
            }
            if (sym->asm_name && strcmp(sym->asm_name, name) != 0) {
                diag_error(a->loc, "conflicting asm labels for '%s'", sym->name);
            }
            free(sym->asm_name);
            sym->asm_name = name;
        }
    }
}

static void sema_declaration(AST *decl, int file_scope) {
    Specs sp;
    size_t i;

    if (static_assertion(decl)) {
        return;
    }
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
        Symbol *sym = id->sym;
        AST *saved_top = array_param_top;
        Type *t;
        Type *base;
        int is_static;
        StorageKind prev;   /* a redeclaration's linkage comes from the earlier one */

        if (sym && sym->kind == SYM_PARAM) {
            array_param_top = declarator_top(id->u.init_decl.declarator);    /* a K&R parameter's */
        }
        base = sp.type;
        if (sp.auto_type) {
            /* __auto_type: the initializer's type, as an rvalue's; it is analysed once, here */
            if (!*init || (*init)->kind == N_INIT_LIST || id->u.init_decl.declarator->kind != N_DECL_NAME || sp.is_typedef) {
                diag_error(id->loc, "'__auto_type' needs a plain name and an initializer that is an expression");
            } else {
                Type *it = sema_rvalue(init);
                if (it->kind == TY_VOID) {
                    diag_error(id->loc, "variable has incomplete type void");
                } else {
                    base = type_qualify(type_unqualified(it), sp.type->quals);
                    if ((*init)->kind != N_IMPLICIT_CAST) {
                        ast_implicit_cast(init, it);    /* marks it analysed */
                    }
                }
            }
        }
        t = sema_declarator(id->u.init_decl.declarator, base, &name);
        t = apply_aligned(t, decl_aligned(id->u.init_decl.declarator, &sp), sp.is_typedef);
        array_param_top = saved_top;

        id->type = t;
        if (!sym || !name) {
            diag_error(id->loc, "internal error: declarator was not declared");
            continue;
        }
        prev = sym->storage;
        sym->storage = sp.storage;
        if (file_scope && !sp.is_typedef) {
            scan_alias_attrs(sym, decl->u.decl.specs, id->u.init_decl.declarator, id->loc);
        }
        if (!sp.is_typedef && (file_scope || sp.storage == STORAGE_EXTERN || t->kind == TY_FUNC || sp.storage == STORAGE_STATIC)) {
            scan_asm_label(sym, id->u.init_decl.declarator);
        }
        if (type_is_vm(t) && t->kind != TY_FUNC) {
            /* C99 6.7.5.2: variably modified types only for names with no linkage at block scope, VLAs only for automatic objects */
            if (file_scope) {
                diag_error(id->loc, "variable length array declaration not allowed at file scope");
                t = type_is_vla(t) ? type_array(t->base, 1) : type_pointer(type_basic(TY_CHAR));
            } else if (sp.storage == STORAGE_EXTERN || (sp.storage == STORAGE_STATIC && type_is_vla(t))) {
                diag_error(id->loc, "variable length array '%s' cannot have %s storage", name,
                           sp.storage == STORAGE_EXTERN ? "extern" : "static");
                t = type_is_vla(t) ? type_array(t->base, 1) : t;
            } else if (*init && type_is_vla(t) && !sp.is_typedef) {
                diag_error(id->loc, "variable-sized object '%s' may not be initialized", name);
                ast_free(*init);
                *init = NULL;
            }
            id->type = t;
        }

        if (sp.is_thread && (sp.is_typedef || t->kind == TY_FUNC)) {
            diag_error(id->loc, "'%s' cannot be thread-local", name);
            sp.is_thread = 0;
        } else if (sp.is_thread && !file_scope && sp.storage != STORAGE_STATIC && sp.storage != STORAGE_EXTERN) {
            diag_error(id->loc, "thread-local variable '%s' at block scope must also be static or extern", name);
            sp.is_thread = 0;
        }

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
                    sym->storage = prev == STORAGE_STATIC ? STORAGE_STATIC : sp.storage;
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
            if (sym->type->kind == TY_POINTER) {
                sym->type = type_qualify(sym->type, array_param_quals(id->u.init_decl.declarator));
            }
            continue;
        }
        if (sym->type && sym->decl != id && sym->tls != sp.is_thread) {
            diag_error(id->loc, "%s declaration of '%s' follows %s declaration", sp.is_thread ? "thread-local" : "non-thread-local",
                       name, sp.is_thread ? "non-thread-local" : "thread-local");
            diag_note(sym->loc, "previous declaration is here");
            continue;
        }
        sym->tls = sp.is_thread;
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
                sym->storage = prev;    /* `int x = 1; extern int x;` still defines x */
            } else if (prev == STORAGE_EXTERN) {
                sym->storage = sp.storage;
            } else if (sp.storage == STORAGE_STATIC && prev != STORAGE_STATIC) {
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

        case N_ASM_STMT:
            sema_asm(s);
            break;

        case N_FUNCTION_DEF: {
            /* a GNU nested function: checked as a function of its own, then this one goes on */
            Type *saved_return = cur_return;
            ASTList *saved_gotos = cur_gotos;
            int saved_loops = loop_depth;
            SwitchCtx *saved_switch = switch_ctx;
            sema_function(s);
            cur_return = saved_return;
            cur_gotos = saved_gotos;
            loop_depth = saved_loops;
            switch_ctx = saved_switch;
            break;
        }

        case N_GOTO:
            if (s->u.stmt.expr) {
                /* GNU `goto *p` */
                Type *t = sema_rvalue(&s->u.stmt.expr);
                if (t->kind != TY_POINTER && !poisoned(s->u.stmt.expr)) {
                    diag_error(s->loc, "'goto *' needs a pointer, not %s", ts(t));
                }
                break;
            }
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
    if (sym) {
        scan_alias_attrs(sym, fdef->u.func.specs, fdef->u.func.declarator, fdef->loc);
        scan_asm_label(sym, fdef->u.func.declarator);
        if (sym->alias) {
            diag_error(fdef->loc, "a function definition cannot be an alias");
            free(sym->alias);
            sym->alias = NULL;
        }
    }
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
    /* this function from here, its parameters' types included (a nested one's may use the enclosing ones' locals) */
    if (fn_depth == (int)(sizeof(fn_stack) / sizeof(fn_stack[0]))) {
        diag_error(fdef->loc, "functions nested too deeply");
        return;
    }
    fn_stack[fn_depth] = sym;
    fn_scope_stack[fn_depth] = fdef->u.func.scope;
    fn_depth++;
    defining_fn = fn;
    t = sema_declarator(declarator, sp.type, &name);
    defining_fn = NULL;
    if (t->kind != TY_FUNC || !fn || fn->kind != N_DECL_FUNC) {
        diag_error(fdef->loc, "'%s' is not a function declarator", name ? name : "");
        fn_depth--;
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
        {
            /* an aligned attribute on an earlier declaration, or on this one, stays */
            size_t al = decl_aligned(declarator, &sp);
            if (sym->type && sym->type->align > al) {
                al = sym->type->align;
            }
            t = apply_aligned(t, al, 0);
        }
        sym->type = t;
        fdef->type = t;
    }

    if (fn_depth > 1 && sym) {
        /* a GNU nested function: no linkage, and its parent's frame for a static chain */
        sym->parent_fn = fn_stack[fn_depth - 2];
        sym->storage = STORAGE_STATIC;
    }
    cur_return = t->base;
    cur_gotos = NULL;
    loop_depth = 0;
    switch_ctx = NULL;
    sema_stmt(fdef->u.func.body);
    fn_depth--;

    /* labels may be used before they are defined, so gotos resolve last */
    if (cur_gotos) {
        for (i = 0; i < cur_gotos->len; i++) {
            AST *g = cur_gotos->items[i];
            Symbol *label = scope_lookup(fdef->u.func.scope, NS_LABEL, g->u.stmt.name);
            int k;
            for (k = fn_depth - 1; !label && k >= 0; k--) {
                /* a label of an enclosing function (GNU nested functions): a jump out to its frame */
                label = scope_lookup(fn_scope_stack[k], NS_LABEL, g->u.stmt.name);
                if (label) {
                    label->owner_fn = fn_stack[k];
                    label->addr_taken = 1;
                    if (fn_stack[k]) {
                        fn_stack[k]->nonlocal_target = 1;
                    }
                }
            }
            if (!label) {
                diag_error(g->loc, "use of undeclared label '%s'", g->u.stmt.name);
            } else if (g->kind == N_LABEL_ADDR) {
                label->addr_taken = 1;
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
    init_static_ctx = 0;
    ncompound_lits = 0;
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
        } else if (item->kind == N_ATTRIBUTE) {
            free(sema_file_asm(item));
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
    { "__builtin_add_overflow_p", "i(.)" },
    { "__builtin_sub_overflow_p", "i(.)" },
    { "__builtin_mul_overflow_p", "i(.)" },
    /* type generic too: the floating point classification of <math.h> */
    { "__builtin_isnan", "i(.)" },
    { "__builtin_isnanf", "i(.)" },
    { "__builtin_isnanl", "i(.)" },
    { "__builtin_isinf", "i(.)" },
    { "__builtin_isinff", "i(.)" },
    { "__builtin_isinfl", "i(.)" },
    { "__builtin_isinf_sign", "i(.)" },
    { "__builtin_isfinite", "i(.)" },
    { "__builtin_finite", "i(.)" },
    { "__builtin_finitef", "i(.)" },
    { "__builtin_finitel", "i(.)" },
    { "__builtin_isnormal", "i(.)" },
    { "__builtin_signbit", "i(.)" },
    { "__builtin_signbitf", "i(.)" },
    { "__builtin_signbitl", "i(.)" },
    { "__builtin_fpclassify", "i(.)" },
    { "__builtin_isgreater", "i(.)" },
    { "__builtin_isgreaterequal", "i(.)" },
    { "__builtin_isless", "i(.)" },
    { "__builtin_islessequal", "i(.)" },
    { "__builtin_islessgreater", "i(.)" },
    { "__builtin_isunordered", "i(.)" },
    { "__builtin_clz", "i(u)" },
    { "__builtin_clzl", "i(U)" },
    { "__builtin_clzll", "i(q)" },
    { "__builtin_ctz", "i(u)" },
    { "__builtin_ctzl", "i(U)" },
    { "__builtin_ctzll", "i(q)" },
    { "__sync_synchronize", "v(v)" },
    { "__builtin_setjmp", "i(p)" },
    { "__builtin_va_arg_pack", "i(v)" },
    { "__builtin_apply_args", "p(v)" },
    { "__builtin_apply", "p(.)" },
    { "__builtin_return", "v(p)" },
    { "__builtin_va_arg_pack_len", "i(v)" },
    { "__builtin_longjmp", "v(pi)" },
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

static const AtomicBuiltin atomic_builtins[] = {
    { "__atomic_load_n", AK_LOAD_N, "load", 0, 2, 0 },
    { "__atomic_load", AK_LOAD, "load", 0, 3, 0 },
    { "__atomic_store_n", AK_STORE_N, "store", 0, 3, 0 },
    { "__atomic_store", AK_STORE, "store", 0, 3, 0 },
    { "__atomic_exchange_n", AK_XCHG_N, "xchg", 0, 3, 0 },
    { "__atomic_exchange", AK_XCHG, "xchg", 0, 4, 0 },
    { "__atomic_compare_exchange_n", AK_CAS_N, "cas", 0, 6, 0 },
    { "__atomic_compare_exchange", AK_CAS, "cas", 0, 6, 0 },
    { "__atomic_fetch_add", AK_FETCH, "add", 0, 3, 0 },
    { "__atomic_fetch_sub", AK_FETCH, "sub", 0, 3, 0 },
    { "__atomic_fetch_and", AK_FETCH, "and", 0, 3, 0 },
    { "__atomic_fetch_or", AK_FETCH, "or", 0, 3, 0 },
    { "__atomic_fetch_xor", AK_FETCH, "xor", 0, 3, 0 },
    { "__atomic_fetch_nand", AK_FETCH, "nand", 0, 3, 0 },
    { "__atomic_add_fetch", AK_FETCH, "add", 1, 3, 0 },
    { "__atomic_sub_fetch", AK_FETCH, "sub", 1, 3, 0 },
    { "__atomic_and_fetch", AK_FETCH, "and", 1, 3, 0 },
    { "__atomic_or_fetch", AK_FETCH, "or", 1, 3, 0 },
    { "__atomic_xor_fetch", AK_FETCH, "xor", 1, 3, 0 },
    { "__atomic_nand_fetch", AK_FETCH, "nand", 1, 3, 0 },
    { "__atomic_test_and_set", AK_TAS, "xchg", 0, 2, 0 },
    { "__atomic_clear", AK_CLEAR, "store", 0, 2, 0 },
    { "__atomic_thread_fence", AK_FENCE, NULL, 0, 1, 0 },
    { "__atomic_signal_fence", AK_FENCE, NULL, 0, 1, 0 },
    { "__atomic_is_lock_free", AK_LOCK_FREE, NULL, 0, 2, 0 },
    { "__atomic_always_lock_free", AK_LOCK_FREE, NULL, 0, 2, 0 },
    { "__sync_fetch_and_add", AK_FETCH, "add", 0, 2, 0 },
    { "__sync_fetch_and_sub", AK_FETCH, "sub", 0, 2, 0 },
    { "__sync_fetch_and_and", AK_FETCH, "and", 0, 2, 0 },
    { "__sync_fetch_and_or", AK_FETCH, "or", 0, 2, 0 },
    { "__sync_fetch_and_xor", AK_FETCH, "xor", 0, 2, 0 },
    { "__sync_fetch_and_nand", AK_FETCH, "nand", 0, 2, 0 },
    { "__sync_add_and_fetch", AK_FETCH, "add", 1, 2, 0 },
    { "__sync_sub_and_fetch", AK_FETCH, "sub", 1, 2, 0 },
    { "__sync_and_and_fetch", AK_FETCH, "and", 1, 2, 0 },
    { "__sync_or_and_fetch", AK_FETCH, "or", 1, 2, 0 },
    { "__sync_xor_and_fetch", AK_FETCH, "xor", 1, 2, 0 },
    { "__sync_nand_and_fetch", AK_FETCH, "nand", 1, 2, 0 },
    { "__sync_bool_compare_and_swap", AK_SYNC_BOOL_CAS, "cas", 0, 3, 0 },
    { "__sync_val_compare_and_swap", AK_SYNC_VAL_CAS, "cas", 0, 3, 0 },
    { "__sync_lock_test_and_set", AK_XCHG_N, "xchg", 0, 2, 0 },
    { "__sync_lock_release", AK_RELEASE, "store", 0, 1, 0 },
    { "__c11_atomic_init", AK_INIT, NULL, 0, 2, 0 },
    { "__c11_atomic_load", AK_LOAD_N, "load", 0, 2, 0 },
    { "__c11_atomic_store", AK_STORE_N, "store", 0, 3, 0 },
    { "__c11_atomic_exchange", AK_XCHG_N, "xchg", 0, 3, 0 },
    { "__c11_atomic_compare_exchange_strong", AK_CAS_N, "cas", 0, 5, 0 },
    { "__c11_atomic_compare_exchange_weak", AK_CAS_N, "cas", 0, 5, 0 },
    { "__c11_atomic_fetch_add", AK_FETCH, "add", 0, 3, 1 },
    { "__c11_atomic_fetch_sub", AK_FETCH, "sub", 0, 3, 1 },
    { "__c11_atomic_fetch_and", AK_FETCH, "and", 0, 3, 0 },
    { "__c11_atomic_fetch_or", AK_FETCH, "or", 0, 3, 0 },
    { "__c11_atomic_fetch_xor", AK_FETCH, "xor", 0, 3, 0 },
    { "__c11_atomic_fetch_nand", AK_FETCH, "nand", 0, 3, 0 },
    { "__c11_atomic_thread_fence", AK_FENCE, NULL, 0, 1, 0 },
    { "__c11_atomic_signal_fence", AK_FENCE, NULL, 0, 1, 0 },
    { "__c11_atomic_is_lock_free", AK_LOCK_FREE, NULL, 0, 1, 0 },
    { NULL, AK_FENCE, NULL, 0, 0, 0 }
};

const AtomicBuiltin *sema_atomic_builtin(const char *name) {
    const AtomicBuiltin *b;
    if (strncmp(name, "__atomic_", 9) != 0 && strncmp(name, "__sync_", 7) != 0 && strncmp(name, "__c11_atomic_", 13) != 0) {
        return NULL;
    }
    for (b = atomic_builtins; b->name; b++) {
        if (strcmp(b->name, name) == 0) {
            return b;
        }
    }
    return NULL;
}

/* an operand of an atomic builtin that points to an object of `size` bytes (any when 0); the pointee, NULL after an error */
static Type *atomic_pointer(AST *e, AST *arg, const char *name, size_t size) {
    Type *pt = arg->type;
    size_t n;
    if (pt->kind != TY_POINTER || pt->base->kind == TY_FUNC || pt->base->kind == TY_VOID || !type_is_complete(pt->base)) {
        diag_error(arg->loc, "'%s' needs a pointer to a complete object, not %s", name, ts(pt));
        return NULL;
    }
    n = type_size(pt->base);
    if (size ? n != size : (n != 1 && n != 2 && n != 4 && n != 8)) {
        diag_error(size ? arg->loc : e->loc, size ? "'%s': the objects are not all the same size"
                   : "'%s' of a %lu byte object: only 1, 2, 4 and 8 byte objects are supported", name, (unsigned long)n);
        return NULL;
    }
    return pt->base;
}

/*
 * The atomic builtins (sema_atomic_builtin) are generic in the pointee
 * type, so they are not in the table: the call is checked here, and the
 * function symbol only exists to be called.
 */
static Type *sema_atomic(AST *e, const AtomicBuiltin *ab) {
    AST **f = &e->u.call.func;
    ASTList *args = e->u.call.args;
    const char *name = ab->name;
    Symbol *sym;
    Type *obj = NULL;
    Type *t = NULL;
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
    e->type = type_basic(TY_VOID);
    if (args->len < (size_t)ab->nargs || (args->len > (size_t)ab->nargs && strncmp(name, "__sync_", 7) != 0
            && !(ab->kind == AK_LOCK_FREE && args->len == 2))) {
        diag_error(e->loc, "'%s' takes %d arguments", name, ab->nargs);
        return error_type(e);
    }
    if (ab->kind == AK_FENCE) {
        return e->type;
    }
    if (ab->kind == AK_LOCK_FREE) {
        if (!type_is_integer(args->items[0]->type)) {
            diag_error(args->items[0]->loc, "'%s' needs the size of the object", name);
        }
        e->type = type_basic(TY_BOOL);
        return e->type;
    }
    obj = atomic_pointer(e, args->items[0], name, 0);
    if (!obj) {
        return error_type(e);
    }
    t = type_unqualified(obj);
    switch (ab->kind) {
        case AK_LOAD_N:
            e->type = t;
            break;
        case AK_STORE_N:
        case AK_INIT:
        case AK_XCHG_N:
            convert_assign(&args->items[1], t, "storing a value of type");
            e->type = ab->kind == AK_XCHG_N ? t : type_basic(TY_VOID);
            break;
        case AK_FETCH:
            if (!type_is_integer(t) && t->kind != TY_POINTER) {
                diag_error(e->loc, "'%s' needs a pointer to an integer or a pointer, not %s", name, ts(args->items[0]->type));
                return error_type(e);
            }
            convert_assign(&args->items[1], t->kind == TY_POINTER ? type_ptrdiff_t() : t, "passing a value of type");
            e->type = t;
            break;
        case AK_SYNC_BOOL_CAS:
        case AK_SYNC_VAL_CAS:
            convert_assign(&args->items[1], t, "passing a value of type");
            convert_assign(&args->items[2], t, "passing a value of type");
            e->type = ab->kind == AK_SYNC_BOOL_CAS ? type_basic(TY_BOOL) : t;
            break;
        case AK_CAS_N:
            if (!atomic_pointer(e, args->items[1], name, type_size(obj))) {
                return error_type(e);
            }
            convert_assign(&args->items[2], t, "passing a value of type");
            e->type = type_basic(TY_BOOL);
            break;
        case AK_CAS:
        case AK_XCHG:
        case AK_LOAD:
        case AK_STORE:
            for (i = 1; i < (ab->kind == AK_LOAD || ab->kind == AK_STORE ? 2u : 3u); i++) {
                if (!atomic_pointer(e, args->items[i], name, type_size(obj))) {
                    return error_type(e);
                }
            }
            e->type = ab->kind == AK_CAS ? type_basic(TY_BOOL) : type_basic(TY_VOID);
            break;
        case AK_TAS:
            e->type = type_basic(TY_BOOL);
            break;
        default:
            break;
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

char *sema_file_asm(const AST *item) {
    return sema_asm_text(item->u.text, item->loc);
}

char *sema_asm_text(const char *p, Loc loc) {
    char *lits = xmalloc(strlen(p) + 1);
    size_t n = 0;
    char *text;

    /* the keyword and any qualifiers, then ( "..." "..." ): the literals are gathered space-separated, as decode_literal reads them */
    while (*p && *p != '(') {
        p++;
    }
    if (*p++ != '(') {
        goto bad;
    }
    for (;;) {
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == ')') {
            break;
        }
        if (*p != '"') {
            goto bad;
        }
        if (n > 0) {
            lits[n++] = ' ';
        }
        lits[n++] = *p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) {
                lits[n++] = *p++;
            }
            lits[n++] = *p++;
        }
        if (*p != '"') {
            goto bad;
        }
        lits[n++] = *p++;
    }
    if (n == 0) {
        goto bad;
    }
    lits[n] = '\0';
    sema_string_bytes(lits, &text);
    free(lits);
    return text;
bad:
    free(lits);
    diag_error(loc, "a basic asm takes one string literal");
    return NULL;
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
