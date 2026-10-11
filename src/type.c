#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "type.h"
#include "arch.h"
#include "diag.h"
#include "util.h"

/* arena: every Type and Record ever made, so they can be freed at once */
static Type *all_types = NULL;
static Record *all_records = NULL;

/* the unqualified basic types are singletons */
static Type *basic[TY_NKINDS];

static Type *type_alloc(TypeKind kind) {
    Type *t = xcalloc(1, sizeof(*t));
    t->kind = kind;
    t->length = -1;
    t->next_alloc = all_types;
    all_types = t;
    return t;
}

Type *type_basic(TypeKind kind) {
    if (!basic[kind]) {
        basic[kind] = type_alloc(kind);
    }
    return basic[kind];
}

Type *type_pointer(Type *base) {
    Type *t = type_alloc(TY_POINTER);
    t->base = base;
    return t;
}

Type *type_array(Type *elem, long length) {
    Type *t = type_alloc(TY_ARRAY);
    t->base = elem;
    t->length = length;
    return t;
}

Type *type_func(Type *ret, Param *params, size_t nparams, int variadic, int has_proto) {
    Type *t = type_alloc(TY_FUNC);
    t->base = ret;
    t->params = params;
    t->nparams = nparams;
    t->variadic = variadic;
    t->has_proto = has_proto;
    return t;
}

Type *type_record(Record *record) {
    Type *t = type_alloc(record->kind);
    t->record = record;
    return t;
}

Type *type_qualify(Type *t, unsigned quals) {
    Type *q;
    if ((t->quals | quals) == t->quals) {
        return t;
    }
    q = type_alloc(t->kind);
    q->quals = t->quals | quals;
    q->base = t->base;
    q->length = t->length;
    q->record = t->record;
    q->params = t->params;
    q->nparams = t->nparams;
    q->variadic = t->variadic;
    q->has_proto = t->has_proto;
    return q;
}

Type *type_unqualified(Type *t) {
    Type *q;
    if (!t->quals) {
        return t;
    }
    if (t->kind <= TY_VA_LIST) {
        return type_basic(t->kind);
    }
    q = type_alloc(t->kind);
    q->base = t->base;
    q->length = t->length;
    q->record = t->record;
    q->params = t->params;
    q->nparams = t->nparams;
    q->variadic = t->variadic;
    q->has_proto = t->has_proto;
    return q;
}

Record *record_new(TypeKind kind, const char *tag) {
    Record *r = xcalloc(1, sizeof(*r));
    r->kind = kind;
    r->tag = tag ? xstrdup(tag) : NULL;
    r->next_alloc = all_records;
    all_records = r;
    return r;
}

void record_add_member(Record *r, const char *name, Type *type, int bits, struct AST *decl) {
    Member *m;
    r->members = realloc(r->members, (r->nmembers + 1) * sizeof(*r->members));
    if (!r->members) {
        fatal("out of memory");
    }
    m = &r->members[r->nmembers++];
    m->name = name ? xstrdup(name) : NULL;
    m->type = type;
    m->offset = 0;
    m->bits = bits;
    m->bit_offset = 0;
    m->unit = type ? type_size(type) : 0;
    m->decl = decl;
}

/* path[] gets the anonymous members traversed, outermost first; *depth how many */
static Member *lookup_in(const Record *r, const char *name, Member **path, size_t *depth, size_t level) {
    Member *m = record_find(r, name);
    size_t i;
    if (m) {
        *depth = level;
        return m;
    }
    if (level + 1 >= RECORD_PATH_MAX) {
        return NULL;
    }
    for (i = 0; i < r->nmembers; i++) {
        Member *a = &r->members[i];
        if (a->name || a->bits >= 0 || !type_is_record(a->type)) {
            continue;
        }
        path[level] = a;
        m = lookup_in(a->type->record, name, path, depth, level + 1);
        if (m) {
            return m;
        }
    }
    return NULL;
}

Member *record_lookup(const Record *r, const char *name, Member **path, size_t *depth) {
    return lookup_in(r, name, path, depth, 0);
}

int record_has_unaligned(const Record *r) {
    size_t i;
    for (i = 0; i < r->nmembers; i++) {
        const Member *m = &r->members[i];
        if (m->bits >= 0) {
            continue;
        }
        if (m->offset % type_align(m->type) != 0) {
            return 1;
        }
        if (type_is_record(m->type) && record_has_unaligned(m->type->record)) {
            return 1;
        }
    }
    return 0;
}

Member *record_find(const Record *r, const char *name) {
    size_t i;
    for (i = 0; i < r->nmembers; i++) {
        if (r->members[i].name && strcmp(r->members[i].name, name) == 0) {
            return &r->members[i];
        }
    }
    return NULL;
}

static size_t round_up(size_t n, size_t align) {
    return (n + align - 1) / align * align;
}

/*
 * Layout follows the System V and AAPCS conventions, which agree: each
 * member is aligned to its natural alignment, a bit-field is placed in the
 * next unit of its declared type that has room for it without crossing a
 * unit boundary, and the struct is padded to a multiple of its alignment.
 * `#pragma pack(n)` and `__attribute__((packed))` (n = 1) cap every
 * member's alignment at n and, as in gcc and clang, drop the rule that a
 * bit-field stays inside one unit of its type: it starts at the next free
 * bit. Such a bit-field is then accessed through the smallest unit that
 * holds it, placed so the unit stays inside the struct.
 */
void record_layout(Record *r) {
    size_t bitpos = 0;      /* struct: next free bit; union: unused */
    size_t align = 1;
    size_t size = 0;
    size_t maxalign = r->packed ? 1 : r->pack;
    size_t i;

    for (i = 0; i < r->nmembers; i++) {
        Member *m = &r->members[i];
        size_t malign = type_align(m->type);
        size_t msize;
        if (maxalign && malign > maxalign) {
            malign = maxalign;
        }
        /* a flexible array member contributes alignment but no size */
        msize = m->type->kind == TY_ARRAY && m->type->length < 0 ? 0 : type_size(m->type);

        if (align < malign) {
            align = malign;
        }
        if (r->kind == TY_UNION) {
            m->offset = 0;
            m->bit_offset = 0;
            if (m->bits >= 0) {
                msize = (m->bits + 7) / 8;
            }
            if (size < msize) {
                size = msize;
            }
            continue;
        }
        if (m->bits >= 0) {
            size_t unit = msize * 8;
            if (m->bits == 0) {
                /* `int : 0` pads to the next unit */
                bitpos = round_up(bitpos, unit);
                continue;
            }
            if (!maxalign && bitpos % unit + m->bits > unit) {
                bitpos = round_up(bitpos, unit);
            }
            m->offset = bitpos / unit * msize;
            m->bit_offset = (unsigned)(bitpos - m->offset * 8);
            m->unit = msize;
            bitpos += m->bits;
        } else {
            bitpos = round_up(bitpos, malign * 8);
            m->offset = bitpos / 8;
            bitpos += msize * 8;
        }
    }
    if (r->kind != TY_UNION) {
        size = (bitpos + 7) / 8;
    }
    if (r->aligned && align < r->aligned) {
        align = r->aligned;
    }
    r->size = round_up(size, align);
    r->align = align;
    r->complete = 1;
    if (maxalign && r->kind != TY_UNION) {
        /* a packed bit-field may straddle units of its type: pick the unit that holds it */
        for (i = 0; i < r->nmembers; i++) {
            Member *m = &r->members[i];
            size_t first;
            size_t unit;
            if (m->bits <= 0) {
                continue;
            }
            first = m->offset * 8 + m->bit_offset;
            for (unit = 1; unit < 8 && first % 8 + (size_t)m->bits > unit * 8; unit *= 2) {
            }
            m->offset = first / 8;
            if (m->offset + unit > r->size && r->size >= unit) {
                m->offset = r->size - unit;
            }
            m->bit_offset = (unsigned)(first - m->offset * 8);
            m->unit = unit;
        }
    }
}

void type_free_all(void) {
    Type *t = all_types;
    Record *r = all_records;
    size_t i;

    while (t) {
        Type *next = t->next_alloc;
        for (i = 0; i < t->nparams; i++) {
            free(t->params[i].name);
        }
        free(t->params);
        free(t);
        t = next;
    }
    all_types = NULL;
    while (r) {
        Record *next = r->next_alloc;
        for (i = 0; i < r->nmembers; i++) {
            free(r->members[i].name);
        }
        free(r->members);
        free(r->tag);
        free(r);
        r = next;
    }
    all_records = NULL;
    memset(basic, 0, sizeof(basic));
}

/* classification */

int type_is_integer(const Type *t) {
    return (t->kind >= TY_BOOL && t->kind <= TY_UINT128) || t->kind == TY_ENUM;
}

int type_is_unsigned(const Type *t) {
    switch (t->kind) {
        case TY_CHAR:
            return !arch_info->char_is_signed;
        case TY_UCHAR:
        case TY_USHORT:
        case TY_UINT:
        case TY_ULONG:
        case TY_ULLONG:
        case TY_UINT128:
        case TY_BOOL:
            return 1;
        case TY_ENUM:
            return t->record->is_unsigned;
        default:
            return 0;
    }
}

int type_is_floating(const Type *t) {
    return t->kind >= TY_FLOAT16 && t->kind <= TY_LDOUBLE;
}

int type_is_wide(const Type *t) {
    return t->kind == TY_INT128 || t->kind == TY_UINT128;
}

int type_is_complex(const Type *t) {
    return t->kind == TY_CFLOAT || t->kind == TY_CDOUBLE || t->kind == TY_CLDOUBLE;
}

int type_is_arithmetic(const Type *t) {
    return type_is_integer(t) || type_is_floating(t) || type_is_complex(t);
}

Type *type_complex_base(const Type *t) {
    switch (t->kind) {
        case TY_CFLOAT: return type_basic(TY_FLOAT);
        case TY_CDOUBLE: return type_basic(TY_DOUBLE);
        default: return type_basic(TY_LDOUBLE);
    }
}

Type *type_complex_of(const Type *real) {
    switch (real->kind) {
        case TY_FLOAT: return type_basic(TY_CFLOAT);
        case TY_DOUBLE: return type_basic(TY_CDOUBLE);
        default: return type_basic(TY_CLDOUBLE);
    }
}

int type_is_pointer(const Type *t) {
    return t->kind == TY_POINTER;
}

int type_is_scalar(const Type *t) {
    return type_is_arithmetic(t) || t->kind == TY_POINTER;
}

int type_is_record(const Type *t) {
    return t->kind == TY_STRUCT || t->kind == TY_UNION;
}

int type_is_void(const Type *t) {
    return t->kind == TY_VOID;
}

int type_is_void_pointer(const Type *t) {
    return t->kind == TY_POINTER && t->base->kind == TY_VOID;
}

int type_is_func_pointer(const Type *t) {
    return t->kind == TY_POINTER && t->base->kind == TY_FUNC;
}

int type_is_complete(const Type *t) {
    switch (t->kind) {
        case TY_VOID:
        case TY_FUNC:
            return 0;
        case TY_ARRAY:
            return t->length >= 0 && type_is_complete(t->base);
        case TY_STRUCT:
        case TY_UNION:
        case TY_ENUM:
            return t->record->complete;
        default:
            return 1;
    }
}

int type_rank(const Type *t) {
    switch (t->kind) {
        case TY_BOOL:
            return 1;
        case TY_CHAR:
        case TY_SCHAR:
        case TY_UCHAR:
            return 2;
        case TY_SHORT:
        case TY_USHORT:
            return 3;
        case TY_INT:
        case TY_UINT:
            return 4;
        case TY_ENUM:
            return t->record->underlying ? type_rank(t->record->underlying) : 4;
        case TY_LONG:
        case TY_ULONG:
            return 5;
        case TY_LLONG:
        case TY_ULLONG:
            return 6;
        case TY_INT128:
        case TY_UINT128:
            return 7;
        default:
            return 0;
    }
}

/* sizes */

size_t type_size(const Type *t) {
    switch (t->kind) {
        case TY_VOID:
            return 1;   /* GNU extension: sizeof(void) == 1 */
        case TY_BOOL:
        case TY_CHAR:
        case TY_SCHAR:
        case TY_UCHAR:
            return 1;
        case TY_SHORT:
        case TY_USHORT:
            return arch_info->short_size;
        case TY_INT:
        case TY_UINT:
            return arch_info->int_size;
        case TY_ENUM:
            /* an enum with a fixed underlying type has that type's size; an
               incomplete one is int, as gcc and clang have it */
            return t->record->complete ? t->record->size : arch_info->int_size;
        case TY_LONG:
        case TY_ULONG:
            return arch_info->long_size;
        case TY_LLONG:
        case TY_ULLONG:
            return arch_info->llong_size;
        case TY_INT128:
        case TY_UINT128:
            return arch_info->int128_size;
        case TY_FLOAT16:
            return 2;
        case TY_FLOAT:
            return arch_info->float_size;
        case TY_DOUBLE:
            return arch_info->double_size;
        case TY_LDOUBLE:
            return arch_info->ldouble_size;
        case TY_CFLOAT:
            return 2 * arch_info->float_size;
        case TY_CDOUBLE:
            return 2 * arch_info->double_size;
        case TY_CLDOUBLE:
            return 2 * arch_info->ldouble_size;
        case TY_VA_LIST:
            return arch_info->va_list_size;
        case TY_STRUCT:
        case TY_UNION:
            return t->record->size;
        case TY_POINTER:
            return arch_info->ptr_size;
        case TY_ARRAY:
            return t->length < 0 ? 0 : (size_t)t->length * type_size(t->base);
        case TY_FUNC:
            return 1;   /* GNU extension: sizeof a function is 1 */
        case TY_NKINDS:
            break;
    }
    return 0;
}

size_t type_align(const Type *t) {
    switch (t->kind) {
        case TY_INT128:
        case TY_UINT128:
            return arch_info->int128_align;
        case TY_LDOUBLE:
            return arch_info->ldouble_align;
        case TY_CFLOAT:
            return arch_info->float_size;
        case TY_CDOUBLE:
            return arch_info->double_size;
        case TY_CLDOUBLE:
            return arch_info->ldouble_align;
        case TY_VA_LIST:
            return arch_info->va_list_align;
        case TY_STRUCT:
        case TY_UNION:
            return t->record->align ? t->record->align : 1;
        case TY_ENUM:
            return t->record->complete ? t->record->align : arch_info->int_size;
        case TY_ARRAY:
            return type_align(t->base);
        case TY_FUNC:
            return 1;
        default:
            return type_size(t);
    }
}

Type *type_size_t(void) {
    return type_basic(TY_ULONG);
}

Type *type_ptrdiff_t(void) {
    return type_basic(TY_LONG);
}

/* conversions */

Type *type_promote(Type *t) {
    switch (t->kind) {
        case TY_BOOL:
        case TY_CHAR:
        case TY_SCHAR:
        case TY_UCHAR:
        case TY_SHORT:
        case TY_USHORT:
            /* all of these fit in an int on every target we have */
            return type_basic(TY_INT);
        case TY_ENUM:
            if (t->record->underlying && type_rank(t->record->underlying) >= 4) {
                return t->record->underlying;
            }
            return type_basic(TY_INT);
        default:
            return type_unqualified(t);
    }
}

static TypeKind unsigned_of(TypeKind kind) {
    switch (kind) {
        case TY_INT:
            return TY_UINT;
        case TY_LONG:
            return TY_ULONG;
        case TY_LLONG:
            return TY_ULLONG;
        case TY_INT128:
            return TY_UINT128;
        default:
            return kind;
    }
}

static TypeKind signed_of(TypeKind kind) {
    switch (kind) {
        case TY_UINT:
            return TY_INT;
        case TY_ULONG:
            return TY_LONG;
        case TY_ULLONG:
            return TY_LLONG;
        case TY_UINT128:
            return TY_INT128;
        default:
            return kind;
    }
}

Type *type_arith_common(Type *a, Type *b) {
    int ra, rb;
    Type *hi;
    Type *lo;

    if (type_is_complex(a) || type_is_complex(b)) {
        /* C99 6.3.1.8: complex, of the common real type of the two component or real types */
        Type *c = type_arith_common(type_is_complex(a) ? type_complex_base(a) : a,
                                    type_is_complex(b) ? type_complex_base(b) : b);
        return type_complex_of(type_is_floating(c) ? c : type_basic(TY_DOUBLE));
    }
    if (a->kind == TY_LDOUBLE || b->kind == TY_LDOUBLE) {
        return type_basic(TY_LDOUBLE);
    }
    if (a->kind == TY_DOUBLE || b->kind == TY_DOUBLE) {
        return type_basic(TY_DOUBLE);
    }
    if (a->kind == TY_FLOAT || b->kind == TY_FLOAT) {
        return type_basic(TY_FLOAT);
    }
    a = type_promote(a);
    b = type_promote(b);
    ra = type_rank(a);
    rb = type_rank(b);
    if (ra == rb) {
        if (type_is_unsigned(a) || type_is_unsigned(b)) {
            return type_basic(unsigned_of(a->kind));
        }
        return a;
    }
    hi = ra > rb ? a : b;
    lo = ra > rb ? b : a;
    if (type_is_unsigned(hi) || !type_is_unsigned(lo)) {
        return hi;
    }
    /* signed hi, unsigned lo: hi wins if it can hold every value of lo */
    if (type_size(hi) > type_size(lo)) {
        return type_basic(signed_of(hi->kind));
    }
    return type_basic(unsigned_of(hi->kind));
}

Type *type_decay(Type *t) {
    if (t->kind == TY_ARRAY) {
        return type_pointer(t->base);
    }
    if (t->kind == TY_FUNC) {
        return type_pointer(t);
    }
    if (t->kind == TY_VA_LIST && arch_info->va_list_is_array) {
        return type_pointer(type_unqualified(t));
    }
    return t;
}

/* comparison */

int type_compatible(const Type *a, const Type *b) {
    size_t i;

    if (a == b) {
        return 1;
    }
    if (a->kind != b->kind || a->quals != b->quals) {
        return 0;
    }
    switch (a->kind) {
        case TY_POINTER:
            return type_compatible(a->base, b->base);
        case TY_ARRAY:
            if (a->length >= 0 && b->length >= 0 && a->length != b->length) {
                return 0;
            }
            return type_compatible(a->base, b->base);
        case TY_STRUCT:
        case TY_UNION:
        case TY_ENUM:
            return a->record == b->record;
        case TY_FUNC:
            if (!type_compatible(a->base, b->base)) {
                return 0;
            }
            if (!a->has_proto || !b->has_proto) {
                /* an old style declaration is compatible with any
                   prototype whose parameters survive promotion unchanged;
                   checking that is more than it is worth here */
                return 1;
            }
            if (a->nparams != b->nparams || a->variadic != b->variadic) {
                return 0;
            }
            for (i = 0; i < a->nparams; i++) {
                if (!type_compatible_unqual(a->params[i].type, b->params[i].type)) {
                    return 0;
                }
            }
            return 1;
        default:
            return 1;
    }
}

int type_compatible_unqual(const Type *a, const Type *b) {
    Type ua;
    Type ub;
    if (a->quals == b->quals) {
        return type_compatible(a, b);
    }
    ua = *a;
    ub = *b;
    ua.quals = 0;
    ub.quals = 0;
    return type_compatible(&ua, &ub);
}

Type *type_composite(Type *a, Type *b) {
    if (a->kind == TY_ARRAY && a->length < 0 && b->length >= 0) {
        return b;
    }
    if (a->kind == TY_FUNC && !a->has_proto && b->has_proto) {
        return b;
    }
    if (a->kind == TY_POINTER || a->kind == TY_ARRAY || a->kind == TY_FUNC) {
        if (type_composite(a->base, b->base) == b->base) {
            return b;
        }
    }
    return a;
}

/* printing */

const char *type_kind_name(TypeKind kind) {
    static const char *const names[] = {
#define X(name, repr) repr,
        TYPE_KINDS(X)
#undef X
    };
    return (size_t)kind < TY_NKINDS ? names[kind] : "?";
}

/* a growable string */
typedef struct StrBuf {
    char *s;
    size_t len;
    size_t cap;
} StrBuf;

static void buf_add(StrBuf *b, const char *s) {
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        while (cap < b->len + n + 1) {
            cap *= 2;
        }
        b->s = realloc(b->s, cap);
        if (!b->s) {
            fatal("out of memory");
        }
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n + 1);
    b->len += n;
}

static void buf_quals(StrBuf *b, unsigned quals) {
    if (quals & TQ_CONST) {
        buf_add(b, "const ");
    }
    if (quals & TQ_VOLATILE) {
        buf_add(b, "volatile ");
    }
}

/*
 * `inner` is the declarator built so far (the part that goes where the
 * name would). Pointers prepend a `*`, arrays and functions append, and
 * a pointer to an array or function is parenthesised.
 */
static void format_type(StrBuf *out, const Type *t, const char *inner) {
    StrBuf d;
    size_t i;

    memset(&d, 0, sizeof(d));
    switch (t->kind) {
        case TY_POINTER:
            buf_add(&d, "*");
            buf_quals(&d, t->quals);
            if (t->quals && *inner == '\0') {
                d.len--;    /* no trailing space before nothing */
                d.s[d.len] = '\0';
            }
            buf_add(&d, inner);
            if (t->base->kind == TY_ARRAY || t->base->kind == TY_FUNC) {
                StrBuf p;
                memset(&p, 0, sizeof(p));
                buf_add(&p, "(");
                buf_add(&p, d.s);
                buf_add(&p, ")");
                free(d.s);
                d = p;
            }
            format_type(out, t->base, d.s);
            free(d.s);
            return;

        case TY_ARRAY: {
            char n[32];
            buf_add(&d, inner);
            if (t->length >= 0) {
                snprintf(n, sizeof(n), "[%ld]", t->length);
            } else {
                strcpy(n, "[]");
            }
            buf_add(&d, n);
            format_type(out, t->base, d.s);
            free(d.s);
            return;
        }

        case TY_FUNC:
            buf_add(&d, inner);
            buf_add(&d, "(");
            if (!t->has_proto) {
                /* nothing */
            } else if (t->nparams == 0) {
                buf_add(&d, "void");
            } else {
                for (i = 0; i < t->nparams; i++) {
                    StrBuf p;
                    memset(&p, 0, sizeof(p));
                    format_type(&p, t->params[i].type, "");
                    if (i) {
                        buf_add(&d, ", ");
                    }
                    buf_add(&d, p.s);
                    free(p.s);
                }
                if (t->variadic) {
                    buf_add(&d, ", ...");
                }
            }
            buf_add(&d, ")");
            format_type(out, t->base, d.s);
            free(d.s);
            return;

        case TY_STRUCT:
        case TY_UNION:
        case TY_ENUM:
            buf_quals(out, t->quals);
            buf_add(out, type_kind_name(t->kind));
            buf_add(out, " ");
            buf_add(out, t->record->tag ? t->record->tag : "(anonymous)");
            break;

        default:
            buf_quals(out, t->quals);
            buf_add(out, type_kind_name(t->kind));
            break;
    }
    if (*inner) {
        buf_add(out, " ");
        buf_add(out, inner);
    }
}

char *type_to_string(const Type *t) {
    StrBuf b;
    memset(&b, 0, sizeof(b));
    format_type(&b, t, "");
    return b.s;
}

void type_print(const Type *t, FILE *out) {
    char *s = type_to_string(t);
    fputs(s, out);
    free(s);
}

long type_float_bits(long double v, const Type *t) {
    long bits = 0;
    if (type_size(t) == 4) {
        float f = (float)v;
        memcpy(&bits, &f, sizeof(f));
    } else {
        double d = (double)v;
        memcpy(&bits, &d, sizeof(d));
    }
    return bits;
}
