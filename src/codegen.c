/*
 * Code generation, the translation unit half: which functions are
 * emitted, static data from constant initializers, string literals, the
 * record classification the calling convention needs, and the
 * initializer walker that both static and local initializers share. The
 * functions themselves are lowered to IR one at a time by src/lower.c
 * and handed to the backend's instruction selector; static data goes
 * straight to the backend's data primitives (src/arch.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arch.h"
#include "ast.h"
#include "codegen.h"
#include "diag.h"
#include "ir.h"
#include "lower.h"
#include "opt.h"
#include "sema.h"
#include "symtab.h"
#include "type.h"
#include "util.h"

int codegen_dump_ir = 0;
int codegen_opt_level = 0;

static int nstring_labels = 0;    /* string literal labels, unique per file */
static int nstatics = 0;        /* static locals seen, for unique names */

/* string literals are collected and emitted after the code */
typedef struct StringLit {
    int label;
    char *bytes;
    size_t len;         /* including the NUL */
} StringLit;

static StringLit *strings = NULL;
static size_t nstrings = 0;
static size_t strings_cap = 0;

/*
 * A static initializer is flattened into pieces at increasing offsets;
 * gaps between them are zero.
 */
typedef enum PieceKind {
    PIECE_INT,
    PIECE_ADDR,
    PIECE_STRING_ADDR,
    PIECE_BYTES
} PieceKind;

typedef struct Piece {
    PieceKind kind;
    long offset;
    size_t size;
    long value;        /* PIECE_INT, or the addend */
    char *name;             /* PIECE_ADDR */
    int label;              /* PIECE_STRING_ADDR */
    char *bytes;            /* PIECE_BYTES, owned */
} Piece;

static Piece *pieces = NULL;
static size_t npieces = 0;
static size_t pieces_cap = 0;
static int init_static = 0;     /* walking a static initializer rather than a local one */

static void gen_global(Symbol *s);

/* ------------------------------------------------------------------ */
/* the calling convention's view of a type                            */
/* ------------------------------------------------------------------ */

/*
 * Classifying a record for the calling convention (src/arch.h, ArgType):
 * the scalar leaves are walked in offset order, counting them and noting
 * which eightbytes hold floating point, integers or both.
 */
typedef struct Classify {
    TypeKind fkind;     /* the floating type of the leaves seen, TY_VOID before the first */
    int uniform;        /* every leaf so far is floating point of that one type */
    int has_int[2];     /* eightbyte k holds an integer leaf (a bit-field counts) */
    int has_float[2];   /* eightbyte k holds a floating leaf */
    int big_ldouble;    /* a long double wider than 8 bytes: class X87 on x86_64, so memory */
} Classify;

static void classify_mark(int *flags, size_t off, size_t size) {
    size_t k;
    for (k = 0; k < 2; k++) {
        if (off < 8 * (k + 1) && off + size > 8 * k) {
            flags[k] = 1;
        }
    }
}

/* returns the leaf count (a union's is its widest member's) */
static size_t classify_walk(Type *t, size_t off, Classify *c) {
    size_t n = 0;
    size_t i;

    t = type_unqualified(t);
    if (t->kind == TY_ARRAY) {
        size_t esize = type_size(t->base);
        for (i = 0; t->length > 0 && i < (size_t)t->length; i++) {
            n += classify_walk(t->base, off + i * esize, c);
        }
        return n;
    }
    if (type_is_record(t)) {
        Record *r = t->record;
        for (i = 0; i < r->nmembers; i++) {
            Member *m = &r->members[i];
            size_t k;
            if (m->bits >= 0) {
                classify_mark(c->has_int, off + m->offset, m->unit);
                c->uniform = 0;
                k = 1;
            } else {
                k = classify_walk(m->type, off + m->offset, c);
            }
            n = r->kind == TY_UNION ? (k > n ? k : n) : n + k;
        }
        return n;
    }
    if (type_is_complex(t)) {
        Type *base = type_complex_base(t);
        return classify_walk(base, off, c) + classify_walk(base, off + type_size(base), c);
    }
    if (type_is_floating(t) && type_size(t) <= 8) {
        TypeKind k = type_size(t) == 4 ? TY_FLOAT : TY_DOUBLE;
        if (c->fkind == TY_VOID) {
            c->fkind = k;
        } else if (c->fkind != k) {
            c->uniform = 0;
        }
        classify_mark(c->has_float, off, type_size(t));
    } else {
        if (t->kind == TY_LDOUBLE) {
            c->big_ldouble = 1;
        }
        c->uniform = 0;
        classify_mark(c->has_int, off, type_size(t));
    }
    return 1;
}

static void record_arg_type(Type *t, ArgType *at) {
    Classify c;
    size_t n;
    size_t k;

    memset(&c, 0, sizeof(c));
    c.fkind = TY_VOID;
    c.uniform = 1;
    n = classify_walk(t, 0, &c);
    at->size = type_size(t);
    at->align = type_align(t);
    at->kind = record_has_unaligned(t->record) || c.big_ldouble ? ARG_RECORD_MEMORY : ARG_RECORD;
    at->hfa = c.uniform && c.fkind != TY_VOID && n >= 1 && n <= 4 ? n : 0;
    at->hfa_size = at->hfa ? (c.fkind == TY_FLOAT ? 4 : 8) : 0;
    at->sse = 0;
    for (k = 0; k < 2; k++) {
        if (c.has_float[k] && !c.has_int[k]) {
            at->sse |= 1u << k;
        }
    }
}

void codegen_arg_type(Type *t, ArgType *at) {
    memset(at, 0, sizeof(*at));
    if (type_is_record(t)) {
        record_arg_type(t, at);
    } else if (type_is_complex(t)) {
        /* as a struct of its two components: an HFA of two, one or two SSE eightbytes, or x87 */
        size_t cs = type_size(type_complex_base(t));
        at->size = 2 * cs;
        at->align = type_align(t);
        if (cs == 16 && arch_info->ldouble_x87) {
            at->kind = ARG_CX87;
        } else {
            at->kind = ARG_RECORD;
            at->hfa = 2;
            at->hfa_size = cs;
            at->sse = cs == 4 ? 1u : 3u;
        }
    } else if (type_is_floating(t)) {
        at->size = type_size(t);
        at->align = at->size == 16 ? 16 : 8;
        at->kind = ARG_FLOAT;
    } else {
        at->size = type_size(t);
        at->align = type_align(t);
        at->kind = ARG_INT;
    }
}

/* ------------------------------------------------------------------ */
/* symbols                                                            */
/* ------------------------------------------------------------------ */

int codegen_sym_is_local(const Symbol *s) {
    if (s->kind == SYM_PARAM) {
        return 1;
    }
    return s->kind == SYM_VARIABLE && s->scope->kind != SCOPE_FILE
        && s->storage != STORAGE_STATIC && s->storage != STORAGE_EXTERN;
}

int codegen_sym_is_static_local(const Symbol *s) {
    return s->kind == SYM_VARIABLE && s->scope->kind != SCOPE_FILE && s->storage == STORAGE_STATIC;
}

/* the undecorated assembler name; static locals get a unique suffix */
const char *codegen_sym_name(const Symbol *s) {
    static char buf[1024];
    if (codegen_sym_is_static_local(s)) {
        snprintf(buf, sizeof(buf), "%s.%d", s->name, s->label);
        return buf;
    }
    return s->name;
}

/* defined in this translation unit, so its address need not go through the GOT */
int codegen_sym_defined_here(const Symbol *s) {
    if (s->kind == SYM_FUNCTION) {
        return s->def != NULL;
    }
    if (s->scope->kind != SCOPE_FILE) {
        return s->storage == STORAGE_STATIC;
    }
    return s->storage != STORAGE_EXTERN;
}

/* ------------------------------------------------------------------ */
/* strings                                                            */
/* ------------------------------------------------------------------ */

int codegen_add_string(AST *s) {
    StringLit *lit;
    char *bytes;
    size_t n = sema_string_bytes(s->u.text, &bytes);
    size_t w = sema_string_width(s->u.text);

    if (nstrings == strings_cap) {
        strings_cap = strings_cap ? strings_cap * 2 : 32;
        strings = xrealloc(strings, strings_cap * sizeof(*strings));
    }
    lit = &strings[nstrings++];
    lit->label = ++nstring_labels;
    lit->bytes = bytes;
    lit->len = n + w;
    return lit->label;
}

/* ------------------------------------------------------------------ */
/* static initializers                                                */
/* ------------------------------------------------------------------ */

static void add_piece(PieceKind kind, long offset, size_t size) {
    Piece *p;
    if (npieces == pieces_cap) {
        pieces_cap = pieces_cap ? pieces_cap * 2 : 32;
        pieces = xrealloc(pieces, pieces_cap * sizeof(*pieces));
    }
    p = &pieces[npieces++];
    memset(p, 0, sizeof(*p));
    p->kind = kind;
    p->offset = offset;
    p->size = size;
}

typedef struct Addr {
    const char *name;   /* a global, or NULL */
    int label;          /* a string literal, or 0 */
    long addend;
} Addr;

static int static_addr(AST *e, Addr *a);

/* the address of an object with static storage, as an address constant */
static int static_lvalue(AST *e, Addr *a) {
    long v;

    switch (e->kind) {
        case N_IDENT:
            if (!e->sym || codegen_sym_is_local(e->sym)) {
                return 0;
            }
            a->name = xstrdup(codegen_sym_name(e->sym));
            return 1;

        case N_STRING:
            a->label = codegen_add_string(e);
            return 1;

        case N_MEMBER:
            if (!e->u.member.member) {
                return 0;
            }
            if (e->u.member.arrow ? !static_addr(e->u.member.object, a) : !static_lvalue(e->u.member.object, a)) {
                return 0;
            }
            a->addend += (long)e->u.member.member->offset;
            return 1;

        case N_INDEX: {
            AST *p = e->u.index.array;
            AST *i = e->u.index.index;
            if (!type_is_pointer(p->type)) {
                AST *t = p;
                p = i;
                i = t;
            }
            if (!sema_const_eval(i, &v) || !static_addr(p, a)) {
                return 0;
            }
            a->addend += v * (long)type_size(p->type->base);
            return 1;
        }

        case N_UNARY:
            if (e->u.unary.op == OP_DEREF) {
                return static_addr(e->u.unary.operand, a);
            }
            return 0;

        default:
            return 0;
    }
}

/* a pointer valued constant expression: symbol plus addend, or a plain number */
static int static_addr(AST *e, Addr *a) {
    long v;

    switch (e->kind) {
        case N_IMPLICIT_CAST:
        case N_CAST: {
            AST *x = e->u.cast.expr;
            if (x->type->kind == TY_ARRAY || x->type->kind == TY_FUNC) {
                return static_lvalue(x, a);
            }
            if (type_is_integer(x->type)) {
                if (!sema_const_eval(x, &v)) {
                    return 0;
                }
                a->addend += v;
                return 1;
            }
            return static_addr(x, a);
        }

        case N_UNARY:
            if (e->u.unary.op == OP_ADDR) {
                return static_lvalue(e->u.unary.operand, a);
            }
            return 0;

        case N_IDENT:
            return e->sym && e->sym->kind == SYM_FUNCTION && static_lvalue(e, a);

        case N_STRING:
            return static_lvalue(e, a);

        case N_BINARY: {
            AST *l = e->u.binary.left;
            AST *r = e->u.binary.right;
            OpKind op = e->u.binary.op;
            if (op != OP_ADD && op != OP_SUB) {
                return 0;
            }
            if (op == OP_ADD && !type_is_pointer(l->type)) {
                AST *t = l;
                l = r;
                r = t;
            }
            if (!type_is_pointer(l->type) || !sema_const_eval(r, &v) || !static_addr(l, a)) {
                return 0;
            }
            v *= (long)type_size(l->type->base);
            a->addend += op == OP_ADD ? v : -v;
            return 1;
        }

        default:
            if (type_is_integer(e->type) && sema_const_eval(e, &v)) {
                a->addend += v;
                return 1;
            }
            return 0;
    }
}

/* a floating constant as a piece of static data */
static void add_float_piece(long off, Type *t, long double f) {
    if (type_size(t) == 16) {
        unsigned char *enc = xrealloc(NULL, 16);
        lower_ldouble_bytes(f, enc);
        add_piece(PIECE_BYTES, off, 16);
        pieces[npieces - 1].bytes = (char *)enc;
        return;
    }
    add_piece(PIECE_INT, off, type_size(t));
    pieces[npieces - 1].value = type_float_bits(f, t);
}

static void static_scalar(long off, Type *t, AST *e) {
    long v;
    Addr a;

    if (type_is_complex(t)) {
        long double re, im;
        Type *base = type_complex_base(t);
        if (!sema_const_eval_complex(e, &re, &im)) {
            diag_error(e->loc, "initializer element is not a compile-time constant");
            return;
        }
        add_float_piece(off, base, re);
        add_float_piece(off + (long)type_size(base), base, im);
        return;
    }
    if (type_is_record(t) || !lower_check_value_type(e->loc, t)) {
        return;
    }
    if (type_is_integer(t)) {
        if (!sema_const_eval(e, &v)) {
            diag_error(e->loc, "initializer element is not a compile-time constant");
            return;
        }
        if (type_is_wide(t)) {
            /*
             * Constants are evaluated in 64 bits: the high half is the sign
             * of the value, unless it was converted from an unsigned type
             * of up to 64 bits, which has no sign to extend.
             */
            AST *x = e;
            long hi;
            while ((x->kind == N_IMPLICIT_CAST || x->kind == N_CAST) && type_is_wide(x->type)) {
                x = x->u.cast.expr;
            }
            if (type_is_integer(x->type) && type_size(x->type) <= 8 && type_is_unsigned(x->type)) {
                hi = 0;
            } else {
                hi = v < 0 ? -1 : 0;
            }
            add_piece(PIECE_INT, off, 8);
            pieces[npieces - 1].value = v;
            add_piece(PIECE_INT, off + 8, 8);
            pieces[npieces - 1].value = hi;
            return;
        }
        add_piece(PIECE_INT, off, type_size(t));
        pieces[npieces - 1].value = v;
        return;
    }
    if (type_is_floating(t)) {
        long double f;
        if (!sema_const_eval_float(e, &f)) {
            diag_error(e->loc, "initializer element is not a compile-time constant");
            return;
        }
        add_float_piece(off, t, f);
        return;
    }
    memset(&a, 0, sizeof(a));
    if (!static_addr(e, &a)) {
        diag_error(e->loc, "initializer element is not a compile-time constant");
        return;
    }
    if (a.name) {
        add_piece(PIECE_ADDR, off, type_size(t));
        pieces[npieces - 1].name = (char *)a.name;
    } else if (a.label) {
        add_piece(PIECE_STRING_ADDR, off, type_size(t));
        pieces[npieces - 1].label = a.label;
    } else {
        add_piece(PIECE_INT, off, type_size(t));
    }
    pieces[npieces - 1].value = a.addend;
}

/* ------------------------------------------------------------------ */
/* the initializer walker, static and local                           */
/* ------------------------------------------------------------------ */

static void emit_scalar(long off, Type *t, AST *e) {
    if (init_static) {
        static_scalar(off, t, e);
    } else {
        lower_init_scalar(off, t, e);
    }
}

/* a char (or wchar_t) array initialized from a string literal; the array may be shorter */
static void emit_string(long off, Type *array, AST *s) {
    char *bytes;
    size_t w = sema_string_width(s->u.text);
    size_t n = sema_string_bytes(s->u.text, &bytes) + w;

    if (array->length >= 0 && (size_t)array->length * w < n) {
        n = (size_t)array->length * w;
    }
    if (init_static) {
        add_piece(PIECE_BYTES, off, n);
        pieces[npieces - 1].bytes = bytes;
        return;
    }
    lower_init_string(off, s, n);
    free(bytes);
}

/* as in sema: an item's designators are applied one at a time, path_pos counting them */
typedef struct InitCursor {
    ASTList *list;
    size_t pos;
    size_t path_pos;
} InitCursor;

static int pieces_unordered = 0;    /* a designator was used: pieces may be out of order */

static AST **gen_cur_slot(InitCursor *c) {
    AST **slot = &c->list->items[c->pos];
    if ((*slot)->kind == N_DESIGNATED) {
        return &(*slot)->u.desig.init;
    }
    return slot;
}

static AST *gen_cur_designator(InitCursor *c) {
    AST *item = c->list->items[c->pos];
    if (item->kind == N_DESIGNATED && c->path_pos < item->u.desig.path->len) {
        return item->u.desig.path->items[c->path_pos];
    }
    return NULL;
}

static int gen_cur_is_fresh_designation(InitCursor *c) {
    return c->list->items[c->pos]->kind == N_DESIGNATED && c->path_pos == 0;
}

static void gen_advance(InitCursor *c) {
    c->pos++;
    c->path_pos = 0;
}

/* the designated index or member, which sema has checked */
static long gen_designated_index(AST *d) {
    long v = 0;
    sema_const_eval(d->u.cast.expr, &v);
    return (long)v;
}

static long gen_designated_member(AST *d, Record *r) {
    size_t i;
    for (i = 0; i < r->nmembers; i++) {
        if (r->members[i].name && strcmp(r->members[i].name, d->u.name) == 0) {
            return (long)i;
        }
    }
    return -1;
}

static void gen_init_object(Type *t, InitCursor *c, long off, int top);

/* the single byte piece at `off`, created in order if there is none yet */
static Piece *byte_piece(long off) {
    size_t i = npieces;
    while (i > 0 && pieces[i - 1].offset > off) {
        i--;
    }
    if (i > 0 && pieces[i - 1].offset == off && pieces[i - 1].size == 1 && pieces[i - 1].kind == PIECE_INT) {
        return &pieces[i - 1];
    }
    add_piece(PIECE_INT, off, 1);
    if (i < npieces - 1) {
        Piece p = pieces[npieces - 1];
        memmove(&pieces[i + 1], &pieces[i], (npieces - 1 - i) * sizeof(*pieces));
        pieces[i] = p;
    }
    return &pieces[i];
}

/* a bit-field member's initializer; bit-fields sharing a unit are merged byte by byte */
static void init_bitfield(Member *m, InitCursor *c, long off) {
    AST *item = *gen_cur_slot(c);
    unsigned long mask = m->bits >= 64 ? ~0ul : (1ul << m->bits) - 1;
    size_t unit = m->unit;
    long v;
    size_t k;

    gen_advance(c);
    while (item->kind == N_INIT_LIST && item->u.list->len) {
        item = item->u.list->items[0];
    }
    if (item->kind == N_INIT_LIST || !lower_check_value_type(item->loc, m->type)) {
        return;
    }
    if (!init_static) {
        lower_init_bitfield(off, m, item);
        return;
    }
    if (!sema_const_eval(item, &v)) {
        diag_error(item->loc, "initializer element is not a compile-time constant");
        return;
    }
    v = (long)(((unsigned long)v & mask) << m->bit_offset);
    for (k = 0; k < unit; k++) {
        long byte = (v >> (8 * k)) & 0xff;
        if (byte) {
            byte_piece(off + (long)k)->value |= byte;
        }
    }
}

static void gen_init_braced(Type *t, AST *list, long off) {
    InitCursor c;
    c.list = list->u.list;
    c.pos = 0;
    c.path_pos = 0;
    gen_init_object(t, &c, off, 1);
}

/* one element or member taken from the cursor, with C89 brace elision as in sema */
static void gen_init_elem(Type *t, InitCursor *c, long off) {
    AST *item = *gen_cur_slot(c);

    if (gen_cur_designator(c)) {
        gen_init_object(t, c, off, 0);
        return;
    }
    if (item->kind == N_INIT_LIST) {
        gen_advance(c);
        if (type_is_scalar(t)) {
            while (item->kind == N_INIT_LIST && item->u.list->len) {
                item = item->u.list->items[0];
            }
            if (item->kind != N_INIT_LIST) {
                emit_scalar(off, t, item);
            }
        } else {
            gen_init_braced(t, item, off);
        }
        return;
    }
    if (t->kind == TY_ARRAY && item->kind == N_STRING && type_size(t->base) == 1) {
        gen_advance(c);
        emit_string(off, t, item);
        return;
    }
    if (t->kind == TY_ARRAY || type_is_record(t)) {
        if (type_is_record(t) && item->type && type_is_record(item->type)) {
            gen_advance(c);
            emit_scalar(off, t, item);
            return;
        }
        gen_init_object(t, c, off, 0);
        return;
    }
    gen_advance(c);
    emit_scalar(off, t, item);
}

static void gen_init_object(Type *t, InitCursor *c, long off, int top) {
    if (t->kind == TY_ARRAY) {
        long n = 0;
        long size = (long)type_size(t->base);
        while (c->pos < c->list->len) {
            AST *d;
            if (!top && gen_cur_is_fresh_designation(c)) {
                break;
            }
            d = gen_cur_designator(c);
            if (d) {
                n = gen_designated_index(d);
                c->path_pos++;
                pieces_unordered = 1;
            } else if (t->length >= 0 && n >= t->length) {
                break;
            }
            gen_init_elem(t->base, c, off + n * size);
            n++;
        }
        return;
    }
    if (t->kind == TY_STRUCT) {
        Record *r = t->record;
        size_t i = 0;
        while (c->pos < c->list->len) {
            Member *m;
            AST *d;
            if (!top && gen_cur_is_fresh_designation(c)) {
                break;
            }
            d = gen_cur_designator(c);
            if (d) {
                long k = gen_designated_member(d, r);
                if (k < 0) {
                    gen_advance(c);
                    continue;
                }
                i = (size_t)k;
                c->path_pos++;
                pieces_unordered = 1;
            } else if (i >= r->nmembers) {
                break;
            } else if (!r->members[i].name && r->members[i].bits >= 0) {
                i++;    /* unnamed bit-field */
                continue;
            }
            m = &r->members[i];
            if (m->bits >= 0) {
                init_bitfield(m, c, off + (long)m->offset);
            } else {
                gen_init_elem(m->type, c, off + (long)m->offset);
            }
            i++;
        }
        return;
    }
    if (t->kind == TY_UNION) {
        Record *r = t->record;
        if (r->nmembers && c->pos < c->list->len) {
            AST *d;
            size_t i = 0;
            if (!top && gen_cur_is_fresh_designation(c)) {
                return;
            }
            d = gen_cur_designator(c);
            if (d) {
                long k = gen_designated_member(d, r);
                if (k < 0) {
                    gen_advance(c);
                    return;
                }
                i = (size_t)k;
                c->path_pos++;
                pieces_unordered = 1;
            }
            if (r->members[i].bits >= 0) {
                init_bitfield(&r->members[i], c, off + (long)r->members[i].offset);
            } else {
                gen_init_elem(r->members[i].type, c, off + (long)r->members[i].offset);
            }
        }
        return;
    }
    if (c->pos < c->list->len) {
        gen_init_elem(t, c, off);
    }
}

/* an initializer for an object of type `t` at `off` (a frame offset, or 0 for static data) */
static void gen_initializer(Type *t, AST *init, long off) {
    if (init->kind == N_INIT_LIST) {
        if (type_is_scalar(t)) {
            InitCursor c;
            c.list = init->u.list;
            c.pos = 0;
            c.path_pos = 0;
            if (c.list->len) {
                gen_init_elem(t, &c, off);
            }
        } else {
            gen_init_braced(t, init, off);
        }
        return;
    }
    if (t->kind == TY_ARRAY && init->kind == N_STRING) {
        emit_string(off, t, init);
        return;
    }
    emit_scalar(off, t, init);
}

void codegen_local_initializer(Type *t, AST *init, long off) {
    init_static = 0;
    gen_initializer(t, init, off);
}

/* put designated pieces in offset order; of two for the same bytes the later wins, as in C99 */
static void order_pieces(void) {
    size_t i, j;

    for (i = 1; i < npieces; i++) {
        Piece p = pieces[i];
        j = i;
        while (j > 0 && pieces[j - 1].offset > p.offset) {
            pieces[j] = pieces[j - 1];
            j--;
        }
        pieces[j] = p;
    }
    for (i = 0; i + 1 < npieces; i++) {
        Piece *p = &pieces[i];
        Piece *q = &pieces[i + 1];
        if (p->offset + (long)p->size > q->offset) {
            if (p->kind == PIECE_ADDR) {
                free(p->name);
            } else if (p->kind == PIECE_BYTES) {
                free(p->bytes);
            }
            memmove(p, q, (npieces - i - 1) * sizeof(*pieces));
            npieces--;
            i--;
        }
    }
    pieces_unordered = 0;
}

/* write the collected pieces of a static initializer, zero filling gaps */
static void flush_pieces(size_t total) {
    size_t i;
    long cur = 0;

    if (pieces_unordered) {
        order_pieces();
    }
    for (i = 0; i < npieces; i++) {
        Piece *p = &pieces[i];
        if (p->offset < cur) {
            fatal("internal error: static initializer pieces out of order");
        }
        cg->data_zero((size_t)(p->offset - cur));
        switch (p->kind) {
            case PIECE_INT:
                cg->data_int(p->size, p->value);
                break;
            case PIECE_ADDR:
                cg->data_addr(p->name, p->value);
                free(p->name);
                break;
            case PIECE_STRING_ADDR:
                cg->data_string_addr(p->label, p->value);
                break;
            case PIECE_BYTES:
                cg->data_bytes(p->bytes, p->size);
                free(p->bytes);
                break;
        }
        cur = p->offset + (long)p->size;
    }
    if ((long)total > cur) {
        cg->data_zero(total - (size_t)cur);
    }
    npieces = 0;
}

/* a global or static local: initialized data, or zero data for a tentative definition */
static void gen_global(Symbol *s) {
    Type *t = s->type;
    AST *init = s->def ? s->def->u.init_decl.init : NULL;
    size_t size;

    if (!t) {
        return;
    }
    if (t->kind == TY_ARRAY && t->length < 0) {
        t = type_array(t->base, 1);     /* a tentative `int a[];` */
    }
    size = type_size(t);
    if (!init) {
        cg->data_begin(codegen_sym_name(s), s->storage != STORAGE_STATIC, type_align(t), 1);
        cg->data_zero(size ? size : 1);
        return;
    }
    cg->data_begin(codegen_sym_name(s), s->storage != STORAGE_STATIC, type_align(t), 0);
    init_static = 1;
    npieces = 0;
    gen_initializer(t, init, 0);
    init_static = 0;
    flush_pieces(size);
}

void codegen_static_local(Symbol *s) {
    s->label = ++nstatics;
    gen_global(s);
}

/* ------------------------------------------------------------------ */
/* the translation unit                                               */
/* ------------------------------------------------------------------ */

/*
 * Which functions are referenced. A static function nobody uses is not
 * emitted: the system headers define many static inline functions and
 * compiling them all would only bloat the output. Every function with
 * external linkage is a root, as is every file scope initializer.
 */
static void mark_refs(AST *n) {
    size_t i;
    if (!n) {
        return;
    }
    switch (n->kind) {
        case N_IDENT:
            if (n->sym && n->sym->kind == SYM_FUNCTION) {
                n->sym->addr_taken = 1;     /* a direct call's callee is marked below, without this */
            }
            if (n->sym && n->sym->kind == SYM_FUNCTION && !n->sym->used) {
                n->sym->used = 1;
                if (n->sym->def) {
                    mark_refs(n->sym->def->u.func.body);
                }
            }
            break;
        case N_UNARY:
            mark_refs(n->u.unary.operand);
            break;
        case N_BINARY:
        case N_ASSIGN:
            mark_refs(n->u.binary.left);
            mark_refs(n->u.binary.right);
            break;
        case N_COND:
        case N_IF:
            mark_refs(n->u.cond.cond);
            mark_refs(n->u.cond.then);
            mark_refs(n->u.cond.otherwise);
            break;
        case N_CALL: {
            AST *callee = n->u.call.func;
            while (callee->kind == N_IMPLICIT_CAST) {
                callee = callee->u.cast.expr;
            }
            if (callee->kind == N_IDENT && callee->sym && callee->sym->kind == SYM_FUNCTION) {
                Symbol *f = callee->sym;
                if (!f->used) {
                    f->used = 1;
                    if (f->def) {
                        mark_refs(f->def->u.func.body);
                    }
                }
            } else {
                mark_refs(n->u.call.func);
            }
            for (i = 0; i < n->u.call.args->len; i++) {
                mark_refs(n->u.call.args->items[i]);
            }
            break;
        }
        case N_INDEX:
            mark_refs(n->u.index.array);
            mark_refs(n->u.index.index);
            break;
        case N_MEMBER:
            mark_refs(n->u.member.object);
            break;
        case N_CAST:
        case N_IMPLICIT_CAST:
        case N_VA_ARG:
            mark_refs(n->u.cast.expr);
            break;
        case N_INIT_LIST:
            for (i = 0; i < n->u.list->len; i++) {
                mark_refs(n->u.list->items[i]);
            }
            break;
        case N_DESIGNATED:
            mark_refs(n->u.desig.init);
            break;
        case N_STMT_EXPR:
            mark_refs(n->u.stmt.body);
            break;
        case N_COMPOUND:
            for (i = 0; i < n->u.block.items->len; i++) {
                mark_refs(n->u.block.items->items[i]);
            }
            break;
        case N_DECLARATION:
            for (i = 0; i < n->u.decl.declarators->len; i++) {
                mark_refs(n->u.decl.declarators->items[i]->u.init_decl.init);
            }
            break;
        case N_LABEL:
        case N_CASE:
        case N_DEFAULT:
        case N_EXPR_STMT:
        case N_SWITCH:
        case N_WHILE:
        case N_DO:
        case N_RETURN:
            mark_refs(n->u.stmt.expr);
            mark_refs(n->u.stmt.body);
            break;
        case N_FOR:
            mark_refs(n->u.loop.init);
            mark_refs(n->u.loop.cond);
            mark_refs(n->u.loop.step);
            mark_refs(n->u.loop.body);
            break;
        default:
            break;
    }
}

int codegen(AST *tu, SymTab *symtab, FILE *output) {
    int errors_before = diag_error_count();
    Symbol *s;
    size_t i;

    nstring_labels = 0;
    nstatics = 0;
    cg->begin(output);

    for (s = symtab->file->first; s; s = s->next) {
        if (s->ns == NS_ORDINARY && s->kind == SYM_VARIABLE && s->def) {
            mark_refs(s->def->u.init_decl.init);
        }
    }
    for (i = 0; i < tu->u.list->len; i++) {
        AST *item = tu->u.list->items[i];
        if (item->kind == N_FUNCTION_DEF && item->sym && item->sym->storage != STORAGE_STATIC) {
            item->sym->used = 1;
            mark_refs(item->u.func.body);
        }
    }

    /* objects with static storage at file scope, each once */
    for (s = symtab->file->first; s; s = s->next) {
        if (s->ns != NS_ORDINARY || s->kind != SYM_VARIABLE || s->storage == STORAGE_EXTERN) {
            continue;
        }
        gen_global(s);
    }

    if (codegen_opt_level > 0) {
        /* the whole unit first, so that the optimizer can inline across functions; then each in order */
        IRFunc **funcs = xmalloc((tu->u.list->len + 1) * sizeof(IRFunc *));
        char *droppable = xmalloc(tu->u.list->len + 1);
        char *emit = xmalloc(tu->u.list->len + 1);
        int n = 0, k;
        for (i = 0; i < tu->u.list->len; i++) {
            AST *item = tu->u.list->items[i];
            if (item->kind == N_FUNCTION_DEF && (!item->sym || item->sym->used)) {
                IRFunc *f = lower_function(item);
                if (f) {
                    droppable[n] = (char)(item->sym && item->sym->storage == STORAGE_STATIC && !item->sym->addr_taken);
                    funcs[n++] = f;
                }
            }
        }
        opt_unit(funcs, n, droppable, emit);
        for (k = 0; k < n; k++) {
            if (emit[k]) {
                lower_emit(funcs[k]);
            } else {
                ir_func_free(funcs[k]);
            }
        }
        free(funcs);
        free(droppable);
        free(emit);
    } else {
        for (i = 0; i < tu->u.list->len; i++) {
            AST *item = tu->u.list->items[i];
            if (item->kind == N_FUNCTION_DEF && (!item->sym || item->sym->used)) {
                IRFunc *f = lower_function(item);
                if (f) {
                    lower_emit(f);
                }
            }
        }
    }

    for (i = 0; i < nstrings; i++) {
        cg->string(strings[i].label, strings[i].bytes, strings[i].len);
        free(strings[i].bytes);
    }
    free(strings);
    strings = NULL;
    nstrings = strings_cap = 0;
    free(pieces);
    pieces = NULL;
    npieces = pieces_cap = 0;

    cg->end();
    return diag_error_count() - errors_before;
}
