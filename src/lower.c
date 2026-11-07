/*
 * Lowering: the typed tree of one function becomes IR (src/ir.h). Every
 * expression yields a virtual register holding its value; a struct,
 * union or complex valued expression yields its address, an array or a
 * function its address too. Scalar locals whose address is never taken
 * are virtual registers themselves; everything else lives in a frame
 * slot below the frame pointer (Symbol.offset) and is reached through
 * addr_local. After the body is lowered the function is verified, dumped
 * if -fdump-ir asked, allocated (src/regalloc.c) and handed to the
 * backend's instruction selector.
 *
 * C's conversions are explicit here. A register holds a value extended
 * to its IR type (a char is a sign extended i32), so a load of a narrow
 * object extends, a conversion to a narrow type is a sext or zext of the
 * low bits, and arithmetic happens at the width of the promoted type.
 */
#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arch.h"
#include "ast.h"
#include "diag.h"
#include "ir.h"
#include "lower.h"
#include "sema.h"
#include "symtab.h"
#include "type.h"
#include "util.h"

static IRFunc *F = NULL;            /* the function being lowered */
static Symbol *cur_fsym = NULL;     /* and its symbol */

/* GNU __builtin_va_arg_pack: the function being inlined at its call, see inline_call */
typedef struct InlineCtx {
    int *pack_vals;
    int *pack_by_addr;
    Type **pack_types;
    int npack;
    Type *ret;
    int result;         /* a value's register, or a record's slot address; -1 for void */
    int end;            /* the block after the body */
    struct InlineCtx *outer;
} InlineCtx;

static InlineCtx *inline_ctx = NULL;
static const char *calls_prefix = "__builtin_va_arg_pack";     /* what calls_pack looks for */
static int calls_named(const AST *n, const char *prefix);
static void alloc_local(Symbol *s);
static int nested_call_seen = 0;    /* it calls a GNU nested function, or defines one: the optimizer leaves it alone */
static long frame_off = 0;          /* lowest frame slot allocated so far */
static int break_block = -1;
static int continue_block = -1;
static long sret_off = 0;           /* slot holding the hidden return pointer, 0 when there is none */
static int all_in_memory = 0;       /* the function calls setjmp: no local may live in a register */

typedef struct CaseLabel {
    AST *stmt;
    int block;
} CaseLabel;

typedef struct SwitchGen {
    CaseLabel *cases;
    size_t ncases;
    size_t cap;
    int default_block;
} SwitchGen;

static SwitchGen *cur_switch = NULL;

static int expr(AST *e);
static int addr(AST *e);
static void stmt(AST *s);
static void local_decl(AST *decl);
static void assign_locals(AST *s);

static void unsupported(Loc loc, const char *what) {
    diag_error(loc, "%s is not supported by the code generator yet", what);
}

/* ------------------------------------------------------------------ */
/* types                                                              */
/* ------------------------------------------------------------------ */

static int by_address(const Type *t) {
    return type_is_record(t) || type_is_complex(t) || t->kind == TY_FLOAT128 || type_is_vector(t) || type_is_decimal(t);
}

/* types whose values live in registers: scalars but complex ones, and a va_list that is a plain pointer */
static int is_value_type(const Type *t) {
    return (type_is_scalar(t) && !type_is_complex(t) && t->kind != TY_FLOAT128 && !type_is_decimal(t))
        || (t->kind == TY_VA_LIST && !arch_info->va_list_is_array);
}

/* the IR type a value of C type `t` has in a register; records, arrays and functions are addresses */
static IRType ty(const Type *t) {
    size_t n;
    if (type_is_floating(t)) {
        n = type_size(t);
        return n == 4 ? IR_F32 : n == 8 ? IR_F64 : IR_F80;
    }
    if (type_is_integer(t)) {
        n = type_size(t);
        return n <= 4 ? IR_I32 : n == 8 ? IR_I64 : IR_I128;
    }
    return IR_I64;
}

int lower_check_value_type(Loc loc, const Type *t) {
    if (t->kind == TY_LDOUBLE && arch_info->ldouble_size > 8 && !arch_info->ldouble_x87) {
        unsupported(loc, "long double");
        return 0;
    }
    if (t->kind == TY_FLOAT16) {
        unsupported(loc, "_Float16");
        return 0;
    }
    if ((t->kind == TY_FLOAT128 || type_is_decimal(t)) && strcmp(arch_info->name, "x86_64") != 0) {
        unsupported(loc, t->kind == TY_FLOAT128 ? "_Float128" : "a decimal floating type");     /* (Apple's arm64 has none) */
        return 0;
    }
    if (type_is_complex(t)) {
        unsupported(loc, "_Complex");
        return 0;
    }
    return 1;
}

/* a value passed to or returned from a function; records and complex values by value are allowed */
static int check_arg_type(Loc loc, const Type *t) {
    if (by_address(t)) {
        return 1;
    }
    return lower_check_value_type(loc, t);
}

/* the operand type decides signedness; pointers compare unsigned */
static int op_unsigned(const Type *t) {
    return t->kind == TY_POINTER || type_is_unsigned(t);
}

/* the local `s` lives in a virtual register: which, or -1 */
static int local_vreg(const Symbol *s) {
    return s->vreg > 0 ? s->vreg - 1 : -1;
}

/* ------------------------------------------------------------------ */
/* small emitters                                                     */
/* ------------------------------------------------------------------ */

static int iconst(IRType t, long v) {
    return ir_iconst(F, t, v);
}

/* a jump that structured statements need, unless the block already ended (a return or goto before it) */
static void jump(int block) {
    if (F->cur >= 0) {
        ir_br(F, block);
    }
}

static int addr_local(long off) {
    int dst = ir_vreg(F, IR_I64);
    IRInst *i = ir_inst(F, ir_emit(F, IR_ADDR_LOCAL, IR_I64, dst, -1, -1));
    i->imm = off;
    return dst;
}

static int addr_global(const Symbol *s) {
    int dst = ir_vreg(F, IR_I64);
    IRInst *i = ir_inst(F, ir_emit(F, s->tls ? IR_TLS_ADDR : IR_ADDR_GLOBAL, IR_I64, dst, -1, -1));
    i->sym = ir_name(F, codegen_sym_name(s));
    if (!codegen_sym_defined_here(s)) {
        i->flags |= IRF_EXTERN;
    }
    return dst;
}

static int addr_string(int label) {
    int dst = ir_vreg(F, IR_I64);
    IRInst *i = ir_inst(F, ir_emit(F, IR_ADDR_STRING, IR_I64, dst, -1, -1));
    i->imm = label;
    return dst;
}

/* dst = sign or zero extend the low `size` bytes of v to `to` */
static int extend(IROp op, IRType to, int v, size_t size) {
    int dst = ir_vreg(F, to);
    IRInst *i = ir_inst(F, ir_emit(F, op, to, dst, v, -1));
    i->size = (unsigned char)size;
    return dst;
}

static int trunc_to(IRType to, int v) {
    return ir_unop(F, IR_TRUNC, to, v);
}

/* a conversion instruction with a source type: itof, ftoi, ftof, bitcast */
static int convert_inst(IROp op, IRType to, int v, IRType from, int is_signed) {
    int dst = ir_vreg(F, to);
    IRInst *i = ir_inst(F, ir_emit(F, op, to, dst, v, -1));
    i->t2 = (unsigned char)from;
    if (is_signed) {
        i->flags |= IRF_SIGNED;
    }
    return dst;
}

static void copy_bytes(int dst_addr, int src_addr, size_t size) {
    IRInst *i = ir_inst(F, ir_emit(F, IR_COPY, IR_VOID, -1, dst_addr, src_addr));
    i->imm = (long)size;
}

static int size_of(Type *t);
static int emit_call(const char *name, int target, int nargs, const int *args, Type **types, int *by_addr,
                     size_t nfixed, int variadic, Type *ret, int sret_addr, int result_addr);

/* a copy of an object of type t: of a size known at run time (a struct with a variable length member), through memcpy */
static void copy_object(int dst_addr, int src_addr, Type *t) {
    if (type_is_vla(t)) {
        int args[3];
        Type *types[3];
        args[0] = dst_addr;
        args[1] = src_addr;
        args[2] = size_of(t);
        types[0] = types[1] = type_pointer(type_basic(TY_VOID));
        types[2] = type_size_t();
        emit_call("memcpy", -1, 3, args, types, NULL, 3, 0, types[0], -1, -1);
        return;
    }
    copy_bytes(dst_addr, src_addr, type_size(t));
}

static void zero_bytes(int addr_v, size_t size) {
    IRInst *i = ir_inst(F, ir_emit(F, IR_ZERO, IR_VOID, -1, addr_v, -1));
    i->imm = (long)size;
}

/* ------------------------------------------------------------------ */
/* variable length arrays                                             */
/* ------------------------------------------------------------------ */

/*
 * A VLA type's byte size is computed when its declarator is reached and
 * kept in a frame slot of its own (Vla.slot, src/type.h); every size the
 * code needs for it, sizeof and the scaling of pointer arithmetic, is
 * loaded from there. An object of VLA type is a 16 byte slot: its
 * address (so it is reached like a record passed by reference, through
 * Symbol.by_ref), then the stack pointer from before its first
 * allocation, zero until then. Reaching the declaration again (a loop,
 * a backward goto) returns the stack to that point before allocating:
 * everything allocated since is out of its lifetime, so a loop does not
 * grow the stack. As in GCC, memory alloca returned after it goes too.
 */

/* sizeof(t), an i64 */
static int size_of(Type *t) {
    if (type_is_record(t) && t->record->vm) {
        return ir_load(F, IR_I64, addr_local(t->record->size_slot), 8, 0);
    }
    if (type_is_vla(t)) {
        return ir_load(F, IR_I64, addr_local(t->vla->slot), 8, 0);
    }
    return iconst(IR_I64, (long)type_size(t));
}

/* `n` elements of type t in bytes; n is an i64 */
static int scale_by(int n, Type *t) {
    if (type_is_vla(t)) {
        return ir_binop(F, IR_MUL, IR_I64, n, size_of(t));
    }
    if (type_size(t) == 1) {
        return n;
    }
    return ir_binop_imm(F, IR_MUL, IR_I64, n, (long)type_size(t));
}

static void assign_temps(AST *e);
static long alloc_slot(size_t size, size_t align);
static int temp_addr(const AST *e);
static int convert(int v, Type *from, Type *to);
static int label_block(Symbol *s);

/* a frame slot for the size of each VLA a declarator chain makes (the parameters of a function declarator in it are not its own) */
static void vla_slots(AST *d) {
    for (; d; d = d->u.declarator.inner) {
        if (d->kind == N_DECL_ARRAY && d->type && type_is_vla(d->type)) {
            d->type->vla->slot = alloc_slot(8, 8);
            assign_temps(d->type->vla->size);
        }
        if (d->kind == N_DECL_NAME) {
            break;
        }
    }
}

/* compute the sizes of the VLAs a declarator chain makes, element types first */
static void vla_eval(AST *d) {
    for (; d; d = d->u.declarator.inner) {
        if (d->kind == N_DECL_ARRAY && d->type && type_is_vla(d->type)) {
            Type *t = d->type;
            AST *n = t->vla->size;
            int v = convert(expr(n), n->type, type_basic(TY_ULONG));
            int slot = addr_local(t->vla->slot);
            ir_store(F, IR_I64, slot, scale_by(v, t->base), 8);
        }
        if (d->kind == N_DECL_NAME) {
            break;
        }
    }
}

/* allocate the VLA object `s` on the stack, returning the stack to where its last allocation started */
static void vla_alloc(Symbol *s) {
    int old = ir_load(F, IR_I64, addr_local(s->offset + 8), 8, 0);
    int restore = ir_block(F);
    int save = ir_block(F);
    int join = ir_block(F);
    int sp;
    int addr;
    ir_condbr(F, ir_icmp_imm(F, IR_NE, IR_I64, old, 0), restore, save);
    ir_place(F, restore);
    ir_emit(F, IR_SPRESTORE, IR_VOID, -1, old, -1);
    ir_br(F, join);
    ir_place(F, save);
    sp = ir_vreg(F, IR_I64);
    ir_emit(F, IR_SPSAVE, IR_I64, sp, -1, -1);
    ir_store(F, IR_I64, addr_local(s->offset + 8), sp, 8);
    ir_br(F, join);
    ir_place(F, join);
    addr = addr_local(s->offset);
    ir_store(F, IR_I64, addr, ir_unop(F, IR_ALLOCA, IR_I64, size_of(s->type)), 8);
}

/*
 * The x87 80 bit encoding of `v`: a 64 bit mantissa with its integer bit
 * explicit, then the sign and a 15 bit exponent biased by 16383. Built
 * from the host value with frexp, so it carries whatever precision the
 * host's long double has (a double on Apple arm64).
 */
static void ldouble_encode(long double v, unsigned long *mant_out, unsigned *se_out) {
    unsigned long mant = 0;
    unsigned se = 0;

    if (copysignl(1.0L, v) < 0) {
        se = 0x8000;
        v = -v;
    }
    if (v != v) {
        se |= 0x7fff;
        mant = 0xc000000000000000UL;    /* a quiet NaN */
    } else if (v - v != 0) {
        se |= 0x7fff;
        mant = 0x8000000000000000UL;    /* infinity */
    } else if (v != 0) {
        int exp;
        long double m = frexpl(v, &exp);    /* v = m * 2^exp, m in [0.5, 1) */
        long biased = exp - 1 + 16383;
        mant = (unsigned long)ldexpl(m, 64);
        if (biased >= 0x7fff) {
            biased = 0x7fff;
            mant = 0x8000000000000000UL;
        } else if (biased <= 0) {
            long shift = 1 - biased;
            mant = shift >= 64 ? 0 : mant >> shift;
            biased = 0;
        }
        se |= (unsigned)biased;
    }
    *mant_out = mant;
    *se_out = se;
}

/* the 16 bytes of an x87 long double, little endian, for static data */
/*
 * The IEEE binary128 encoding of `v` (_Float128), little endian: a sign,
 * a 15 bit exponent biased by 16383 and 112 fraction bits. Every value
 * of the host's long double (x87, or a double on Apple arm64) has one.
 */
void lower_f128_bytes(long double v, unsigned char out[16]) {
    unsigned long hi = 0, lo = 0;
    int i;
    if (v != v) {
        hi = 0x7fff800000000000ul;                          /* a quiet NaN */
    } else {
        int neg = signbit(v) != 0;
        long double a = neg ? -v : v;
        if (a == 0) {
            hi = 0;
        } else if (a > LDBL_MAX) {
            hi = 0x7fff000000000000ul;                      /* infinity */
        } else {
            int e;
            long double m = frexpl(a, &e);                  /* a = m * 2^e, m in [0.5, 1) */
            long biased = (long)e - 1 + 16383;
            long double f;
            if (biased <= 0) {
                /* subnormal: no implicit bit, the fraction scaled down */
                f = ldexpl(m, (int)biased);
                biased = 0;
                f = ldexpl(f, 1);
            } else {
                f = 2 * m - 1;                              /* the fraction, below the implicit 1 */
            }
            f = ldexpl(f, 48);
            hi = (unsigned long)f;
            f = ldexpl(f - (long double)hi, 64);
            lo = (unsigned long)f;
            hi |= (unsigned long)biased << 48;
        }
        if (neg) {
            hi |= 1ul << 63;
        }
    }
    for (i = 0; i < 8; i++) {
        out[i] = (unsigned char)(lo >> (8 * i));
        out[8 + i] = (unsigned char)(hi >> (8 * i));
    }
}

/*
 * The BID encoding (IEEE 754 decimal, binary integer significand, as
 * GCC's on x86_64) of the decimal floating constant `text` (digits, a
 * point, an exponent; a suffix after is ignored) in the decimal type
 * `kind`, little endian, size bytes: the significand rounded half to even
 * to the type's 7, 16 or 34 digits.
 */
void lower_bid_bytes(const char *text, int neg, int kind, unsigned char out[16]) {
    int maxd = kind == TY_DECIMAL32 ? 7 : kind == TY_DECIMAL64 ? 16 : 34;
    int bias = kind == TY_DECIMAL32 ? 101 : kind == TY_DECIMAL64 ? 398 : 6176;
    int emax = kind == TY_DECIMAL32 ? 191 : kind == TY_DECIMAL64 ? 767 : 12287;   /* biased exponent's largest */
    unsigned int w[4] = { 0, 0, 0, 0 };     /* the significand, 32 bits a word, least significant first */
    int ndig = 0, exp10 = 0, seen_point = 0, round_digit = -1, sticky = 0;
    const char *p = text;
    long e;
    int k;
    for (; *p && (isdigit((unsigned char)*p) || *p == '.'); p++) {
        if (*p == '.') {
            seen_point = 1;
            continue;
        }
        if (ndig == 0 && *p == '0') {
            if (seen_point) {
                exp10--;
            }
            continue;
        }
        if (ndig < maxd) {
            /* w = w * 10 + digit */
            unsigned long long carry = (unsigned long long)(*p - '0');
            for (k = 0; k < 4; k++) {
                unsigned long long x = (unsigned long long)w[k] * 10 + carry;
                w[k] = (unsigned int)x;
                carry = x >> 32;
            }
            ndig++;
            if (seen_point) {
                exp10--;
            }
        } else {
            /* past the type's digits: kept for the rounding */
            if (round_digit < 0) {
                round_digit = *p - '0';
            } else if (*p != '0') {
                sticky = 1;
            }
            if (!seen_point) {
                exp10++;
            }
        }
    }
    if (*p == 'e' || *p == 'E') {
        e = strtol(p + 1, NULL, 10);
        exp10 += (int)e;
    }
    if (round_digit > 5 || (round_digit == 5 && (sticky || (w[0] & 1)))) {
        for (k = 0; k < 4 && ++w[k] == 0; k++) {
        }
        /* 10^maxd after rounding up: one digit fewer, the exponent one more (w is then exactly 10^maxd) */
    }
    e = exp10 + bias;
    if (e < 0) {
        e = 0;
    }
    if (e > emax) {
        e = emax;
    }
    memset(out, 0, 16);
    if (kind == TY_DECIMAL128) {
        unsigned long lo = (unsigned long)w[0] | (unsigned long)w[1] << 32;
        unsigned long hi = (unsigned long)w[2] | (unsigned long)w[3] << 32;
        hi |= (unsigned long)e << 49;
        if (neg) {
            hi |= 1ul << 63;
        }
        for (k = 0; k < 8; k++) {
            out[k] = (unsigned char)(lo >> (8 * k));
            out[8 + k] = (unsigned char)(hi >> (8 * k));
        }
    } else if (kind == TY_DECIMAL64) {
        unsigned long c = (unsigned long)w[0] | (unsigned long)w[1] << 32;
        unsigned long v = c < (1ul << 53) ? (unsigned long)e << 53 | c
                                            : 3ul << 61 | (unsigned long)e << 51 | (c & ((1ul << 51) - 1));
        if (neg) {
            v |= 1ul << 63;
        }
        for (k = 0; k < 8; k++) {
            out[k] = (unsigned char)(v >> (8 * k));
        }
    } else {
        unsigned long c = w[0];
        unsigned long v = c < (1ul << 23) ? (unsigned long)e << 23 | c : 3ul << 29 | (unsigned long)e << 21 | (c & ((1ul << 21) - 1));
        if (neg) {
            v |= 1ul << 31;
        }
        for (k = 0; k < 4; k++) {
            out[k] = (unsigned char)(v >> (8 * k));
        }
    }
}

void lower_ldouble_bytes(long double v, unsigned char out[16]) {
    unsigned long mant;
    unsigned se;
    int i;
    ldouble_encode(v, &mant, &se);
    memset(out, 0, 16);
    for (i = 0; i < 8; i++) {
        out[i] = (unsigned char)(mant >> (8 * i));
    }
    out[8] = (unsigned char)se;
    out[9] = (unsigned char)(se >> 8);
}

static int float_const(long double v, const Type *t) {
    IRType it = ty(t);
    int dst = ir_vreg(F, it);
    IRInst *i = ir_inst(F, ir_emit(F, IR_FCONST, it, dst, -1, -1));
    if (it == IR_F80) {
        unsigned long mant;
        unsigned se;
        ldouble_encode(v, &mant, &se);
        i->imm = (long)mant;
        i->aux = (int)se;
    } else {
        i->imm = type_float_bits(v, t);
    }
    return dst;
}

/* ------------------------------------------------------------------ */
/* conversions                                                        */
/* ------------------------------------------------------------------ */

/* the value in `v` of scalar type `t`, as an i32 that is 0 or 1 */
static int is_soft(const Type *t);
static int soft_arith(OpKind op, Type *st, int a, int b);
static int soft_zero(Type *st);

static int truth(int v, const Type *t) {
    IRType it;
    if (is_soft(t)) {
        return soft_arith(OP_NE, (Type *)t, v, soft_zero((Type *)t));
    }
    it = ty(t);
    if (ir_type_is_float(it)) {
        int zero = float_const(0, t);
        int dst = ir_vreg(F, IR_I32);
        IRInst *i = ir_inst(F, ir_emit(F, IR_FCMP, it, dst, v, zero));
        i->cc = IR_NE;
        return dst;
    }
    if (it == IR_I128) {
        return ir_icmp(F, IR_NE, IR_I128, v, iconst(IR_I128, 0));
    }
    return ir_icmp_imm(F, IR_NE, it, v, 0);
}

/* does every value of integer type `from` keep its bits when it becomes a `to` of the same register width? */
static int int_fits(const Type *from, const Type *to) {
    size_t fs = type_size(from);
    size_t ts = type_size(to);
    int fsigned = !type_is_unsigned(from) && from->kind != TY_POINTER;
    int tsigned = !type_is_unsigned(to);
    if (fsigned) {
        return tsigned && ts >= fs;
    }
    return ts > fs || (ts == fs && !tsigned);
}

/* `v`, a value of scalar type `from`, converted to `to` */
static int canon(int v, const Type *t);

static int convert_basic(int v, Type *from, Type *to) {
    int ff = type_is_floating(from);
    int tf = type_is_floating(to);
    IRType ft = ty(from);
    IRType tt = ty(to);
    size_t ts = type_size(to);

    if (to->kind == TY_BOOL) {
        if (from->kind == TY_BOOL) {
            return v;
        }
        return truth(v, from);
    }
    if (ff && tf) {
        return ft == tt ? v : convert_inst(IR_FTOF, tt, v, ft, 0);
    }
    if (ff) {
        int r = convert_inst(IR_FTOI, tt, v, ft, !type_is_unsigned(to));
        if (ts < 4) {
            r = extend(type_is_unsigned(to) ? IR_ZEXT : IR_SEXT, IR_I32, r, ts);
        }
        return r;
    }
    if (tf) {
        return convert_inst(IR_ITOF, tt, v, ft, from->kind != TY_POINTER && !type_is_unsigned(from));
    }
    /* integer to integer (pointers are unsigned 8 byte integers here) */
    if (tt == IR_I128) {
        if (ft == IR_I128) {
            return v;
        }
        return extend(from->kind != TY_POINTER && !type_is_unsigned(from) ? IR_SEXT : IR_ZEXT, IR_I128, v, ft == IR_I64 ? 8 : 4);
    }
    if (tt == IR_I64) {
        if (ft == IR_I64) {
            return v;
        }
        if (ft == IR_I128) {
            return trunc_to(IR_I64, v);
        }
        return extend(!type_is_unsigned(from) ? IR_SEXT : IR_ZEXT, IR_I64, v, 4);
    }
    /* to i32 */
    if (ts < 4) {
        if (ft == IR_I32 && int_fits(from, to)) {
            return v;
        }
        return extend(type_is_unsigned(to) ? IR_ZEXT : IR_SEXT, IR_I32, v, ts);
    }
    if (ft == IR_I32) {
        return v;
    }
    return trunc_to(IR_I32, v);
}

static int convert(int v, Type *from, Type *to) {
    v = convert_basic(v, from, to);
    if (to->bits && (from->bits != to->bits || type_is_unsigned(from) != type_is_unsigned(to) || type_is_floating(from))) {
        v = canon(v, to);   /* into a gcc bit-field type: reduced to its width */
    }
    return v;
}

/* a narrow integer result re-extended to its type's canonical form; one of a gcc bit-field type (Type.bits) reduced to its width */
static int canon(int v, const Type *t) {
    size_t n;
    if (!type_is_integer(t) || t->kind == TY_BOOL) {
        return v;
    }
    if (t->bits && ty(t) == IR_I64 && t->bits < 64) {
        if (type_is_unsigned(t)) {
            return ir_binop(F, IR_AND, IR_I64, v, iconst(IR_I64, (long)((1ul << t->bits) - 1)));
        }
        v = ir_binop_imm(F, IR_SHL, IR_I64, v, 64 - t->bits);
        return ir_binop_imm(F, IR_ASHR, IR_I64, v, 64 - t->bits);
    }
    n = type_size(t);
    if (n < 4) {
        return extend(type_is_unsigned(t) ? IR_ZEXT : IR_SEXT, IR_I32, v, n);
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* memory                                                             */
/* ------------------------------------------------------------------ */

/* load the scalar of type `t` at the address in `a` */
/* the access just emitted is to an object of type `t`: a volatile one is marked, so that the optimizer keeps it */
static void mark_volatile(const Type *t) {
    if (t->quals & TQ_VOLATILE) {
        ir_inst(F, ir_last(F))->flags |= IRF_VOLATILE;
    }
}

static int atomic_call(const char *op, size_t size, int p, int v, int w);
static int atomic_to_bits(int v, Type *t);
static int atomic_from_bits(int v, Type *t);
static Type *atomic_bits_type(size_t size);

/* an _Atomic scalar that the atomic helpers can read and write: 1, 2, 4 or 8 bytes */
static int atomic_object(const Type *t) {
    size_t n;
    if (!(t->quals & TQ_ATOMIC) || !is_value_type(t) || type_is_complex(t)) {
        return 0;
    }
    n = type_size(t);
    return n == 1 || n == 2 || n == 4 || n == 8;
}

/*
 * Read-modify-write of an _Atomic object: the old value is read, the new
 * one computed from it and stored with a compare and exchange, again from
 * what was found there if another thread changed it in between. The right
 * operand is evaluated once, before.
 */
typedef struct AtomicLoop {
    int addr;
    Type *t;
    size_t size;
    int cur;        /* the value the object is believed to hold */
    int top;        /* the loop's block */
} AtomicLoop;

static void atomic_loop_begin(AtomicLoop *lp, int a, Type *t) {
    lp->addr = a;
    lp->t = t;
    lp->size = type_size(t);
    lp->cur = ir_vreg(F, ty(t));
    ir_mov(F, lp->cur, atomic_from_bits(atomic_call("load", lp->size, a, -1, -1), t));
    lp->top = ir_block(F);
    ir_br(F, lp->top);
    ir_place(F, lp->top);
}

/* `nw`, computed from lp->cur, is the value to store: loop until it is */
static void atomic_loop_end(AtomicLoop *lp, int nw) {
    int expected = atomic_to_bits(lp->cur, lp->t);
    int got = atomic_call("cas", lp->size, lp->addr, expected, atomic_to_bits(nw, lp->t));
    int ok = ir_icmp(F, IR_EQ, ty(atomic_bits_type(lp->size)), got, expected);
    int done = ir_block(F);
    ir_mov(F, lp->cur, atomic_from_bits(got, lp->t));
    ir_condbr(F, ok, done, lp->top);
    ir_place(F, done);
}

static int load_value(Loc loc, Type *t, int a) {
    int v;
    if (!is_value_type(t) || !lower_check_value_type(loc, t)) {
        return a;       /* a record or array: its address is the value */
    }
    if (atomic_object(t)) {
        return atomic_from_bits(atomic_call("load", type_size(t), a, -1, -1), t);
    }
    v = ir_load(F, ty(t), a, type_size(t), type_is_integer(t) && !type_is_unsigned(t));
    mark_volatile(t);
    return v;
}

static void store_value(Type *t, int a, int v) {
    if (atomic_object(t)) {
        atomic_call("store", type_size(t), a, atomic_to_bits(v, t), -1);
        return;
    }
    ir_store(F, ty(t), a, v, type_size(t));
    mark_volatile(t);
}

/* `t` is the lvalue's type, which carries the qualifiers of the record it is a member of */
/* the `size` bytes at a in reverse order (scalar_storage_order), as an integer of IR type it, zero extended */
static int load_swapped_word(int a, size_t size, IRType it) {
    int v = ir_load(F, it, a, size, 0);
    IRInst *i;
    int dst;
    if (size == 1) {
        return v;
    }
    dst = ir_vreg(F, it);
    i = ir_inst(F, ir_emit(F, IR_BSWAP, it, dst, v, -1));
    i->size = (unsigned char)size;
    return dst;
}

static void store_swapped_word(int a, size_t size, IRType it, int v) {
    if (size > 1) {
        int dst = ir_vreg(F, it);
        IRInst *i = ir_inst(F, ir_emit(F, IR_BSWAP, it, dst, v, -1));
        i->size = (unsigned char)size;
        v = dst;
    }
    ir_store(F, it, a, v, size);
}

/* a bit-field of a reversed struct: its unit byte swapped, the field counted from the unit's top */
static int bits_swapped(Member *m, int a, int v, int store) {
    IRType ut = m->unit > 4 ? IR_I64 : IR_I32;
    int w = (int)(m->unit * 8);
    int sh = w - (int)m->bit_offset - m->bits;
    unsigned long mask = m->bits >= 64 ? ~0ul : (1ul << m->bits) - 1;
    int u = load_swapped_word(a, m->unit, ut);
    IRType ft = ty(m->type);
    int field;
    if (store) {
        int nv = ft == ut ? v : ut == IR_I64 ? extend(IR_ZEXT, IR_I64, v, 4) : trunc_to(IR_I32, v);
        nv = ir_binop_imm(F, IR_SHL, ut, ir_binop(F, IR_AND, ut, nv, iconst(ut, (long)mask)), sh);
        u = ir_binop(F, IR_OR, ut, ir_binop(F, IR_AND, ut, u, iconst(ut, (long)~(mask << sh))), nv);
        store_swapped_word(a, m->unit, ut, u);
    }
    field = ir_binop(F, IR_AND, ut, ir_binop_imm(F, IR_LSHR, ut, u, sh), iconst(ut, (long)mask));
    if (!type_is_unsigned(m->type) && m->bits < w) {
        int up = (ut == IR_I64 ? 64 : 32) - m->bits;
        field = ir_binop_imm(F, IR_ASHR, ut, ir_binop_imm(F, IR_SHL, ut, field, up), up);
    }
    if (ft != ut) {
        field = ft == IR_I64 ? (type_is_unsigned(m->type) ? extend(IR_ZEXT, IR_I64, field, 4) : extend(IR_SEXT, IR_I64, field, 4))
                             : trunc_to(IR_I32, field);
    }
    return field;
}

/* a scalar member of a reversed struct */
static int load_reversed(Type *t, int a) {
    size_t n = type_size(t);
    IRType it = n > 4 ? IR_I64 : IR_I32;
    int v = load_swapped_word(a, n, it);
    if (type_is_floating(t)) {
        int dst = ir_vreg(F, ty(t));
        IRInst *i = ir_inst(F, ir_emit(F, IR_BITCAST, ty(t), dst, v, -1));
        i->t2 = (unsigned char)it;
        return dst;
    }
    if (n < 4 && !type_is_unsigned(t)) {
        v = extend(IR_SEXT, IR_I32, v, n);
    }
    return v;
}

static void store_reversed(Type *t, int a, int v) {
    size_t n = type_size(t);
    IRType it = n > 4 ? IR_I64 : IR_I32;
    if (type_is_floating(t)) {
        int dst = ir_vreg(F, it);
        IRInst *i = ir_inst(F, ir_emit(F, IR_BITCAST, it, dst, v, -1));
        i->t2 = (unsigned char)ty(t);
        v = dst;
    }
    store_swapped_word(a, n, it, v);
}

int lower_init_reverse = 0;    /* the initializer walker is in a member of a reversed struct (src/codegen.c) */

static int load_bits(Member *m, const Type *t, int a) {
    int dst;
    if (m->reverse) {
        return bits_swapped(m, a, -1, 0);
    }
    dst = ir_vreg(F, ty(m->type));
    IRInst *i = ir_inst(F, ir_emit(F, IR_LOADBITS, ty(m->type), dst, a, -1));
    i->size = (unsigned char)m->unit;
    i->imm = (long)m->bit_offset | ((long)m->bits << 8);
    if (!type_is_unsigned(m->type)) {
        i->flags |= IRF_SIGNED;
    }
    mark_volatile(t);
    return dst;
}

/* store `v` into the bit-field; returns the field's value as stored, extended */
static int store_bits(Member *m, const Type *t, int a, int v) {
    int dst;
    if (m->reverse) {
        return bits_swapped(m, a, v, 1);
    }
    dst = ir_vreg(F, ty(m->type));
    IRInst *i = ir_inst(F, ir_emit(F, IR_STOREBITS, ty(m->type), dst, a, v));
    i->size = (unsigned char)m->unit;
    i->imm = (long)m->bit_offset | ((long)m->bits << 8);
    if (!type_is_unsigned(m->type)) {
        i->flags |= IRF_SIGNED;
    }
    mark_volatile(t);
    return dst;
}

/*
 * GNU nested functions. A nested function's static chain is its parent's
 * frame pointer, which it keeps in a slot of its own (chain_off); the
 * frame of a function further out is reached through each one's slot in
 * turn. A local of an enclosing function is at its frame offset there.
 */
static int frame_of(Symbol *fn) {
    Symbol *f = cur_fsym;
    int fp = addr_local(0);
    while (f && f != fn) {
        if (!f->parent_fn) {
            fatal("internal error: a nested function's frame not found");
            return fp;
        }
        fp = ir_load(F, IR_I64, f == cur_fsym ? addr_local(f->chain_off) : ir_binop_imm(F, IR_ADD, IR_I64, fp, f->chain_off), 8, 0);
        f = f->parent_fn;
    }
    return fp;
}

/* an lvalue's address, or -1 for a local that lives in a register */
static int lvalue_addr(AST *l) {
    if (l->kind == N_IDENT && l->sym && local_vreg(l->sym) >= 0) {
        return -1;
    }
    return addr(l);
}

/* the value of the lvalue `l`, whose address is `a` (-1: a register local) */
/* is l (a member, or an element of an array member) in the reversed byte order of scalar_storage_order? */
static int reversed_lvalue(const AST *l) {
    while (l->kind == N_INDEX && l->u.index.array->kind == N_IMPLICIT_CAST
           && l->u.index.array->u.cast.expr->type->kind == TY_ARRAY) {
        l = l->u.index.array->u.cast.expr;
    }
    while (l->kind == N_INDEX && l->u.index.array->type->kind == TY_ARRAY) {
        l = l->u.index.array;
    }
    return l->kind == N_MEMBER && l->u.member.member && l->u.member.member->reverse && type_size(l->type) > 1;
}

static int lvalue_load(AST *l, int a) {
    if (a < 0) {
        return local_vreg(l->sym);
    }
    if (sema_is_bitfield(l)) {
        return load_bits(l->u.member.member, l->type, a);
    }
    if (reversed_lvalue(l) && is_value_type(l->type)) {
        return load_reversed(type_unqualified(l->type), a);
    }
    return load_value(l->loc, l->type, a);
}

/* store `v` into `l`; returns the value of the assignment expression */
static int lvalue_store(AST *l, int a, int v) {
    if (a < 0) {
        ir_mov(F, local_vreg(l->sym), v);
        return local_vreg(l->sym);
    }
    if (sema_is_bitfield(l)) {
        return store_bits(l->u.member.member, l->type, a, v);
    }
    if (reversed_lvalue(l) && is_value_type(l->type)) {
        store_reversed(type_unqualified(l->type), a, v);
        return v;
    }
    store_value(l->type, a, v);
    return v;
}

/* ------------------------------------------------------------------ */
/* addresses                                                          */
/* ------------------------------------------------------------------ */

/* `v` of integer type `t` as an i64 byte offset for elements of `size` bytes */
static int scaled_index(int v, Type *t, size_t size) {
    int idx = convert(v, t, type_basic(type_is_unsigned(t) ? TY_ULONG : TY_LONG));
    if (size == 1) {
        return idx;
    }
    return ir_binop_imm(F, IR_MUL, IR_I64, idx, (long)size);
}

/* the address of `array[index]`; either operand may be the pointer */
static int index_addr(AST *e) {
    AST *a = e->u.index.array;
    AST *i = e->u.index.index;
    size_t size;
    long v;
    int p;

    if (!type_is_pointer(a->type) && !type_is_vector(a->type)) {
        AST *t = a;
        a = i;
        i = t;
    }
    size = type_size(a->type->base);
    p = expr(a);
    if (type_is_vla(a->type->base)) {
        return ir_binop(F, IR_ADD, IR_I64, p, scale_by(scaled_index(expr(i), i->type, 1), a->type->base));
    }
    if (sema_const_eval(i, &v)) {
        if (v == 0) {
            return p;
        }
        return ir_binop_imm(F, IR_ADD, IR_I64, p, (long)((unsigned long)v * size));
    }
    return ir_binop(F, IR_ADD, IR_I64, p, scaled_index(expr(i), i->type, size));
}

/*
 * Where a local's initializer writes. Initializers address the object by
 * frame offset; an over-aligned local (see overaligned_local) or temporary is instead at
 * an address computed at run time, so while its initializer is lowered
 * init_base holds that address and init_base_off the offset it stands for.
 */
static int init_base = -1;
static long init_base_off;

/*
 * A compound literal's object. A static one is data (src/codegen.c); an
 * automatic one owns a frame slot (assign_temps) that is initialized
 * each time the literal is evaluated, as C99 says.
 */
static int compound_lit_addr(AST *e) {
    if (e->sym) {
        return addr_global(e->sym);
    }
    {
        /* the initializer writes relative to this object, even inside an over-aligned local's initializer */
        int saved_base = init_base;
        long saved_off = init_base_off;
        int a = temp_addr(e);
        zero_bytes(a, type_size(e->type));
        init_base = e->temp_by_ref ? a : -1;
        init_base_off = e->temp;
        codegen_local_initializer(e->type, e->u.cast.expr->u.init_decl.init, e->temp);
        init_base = saved_base;
        init_base_off = saved_off;
        return a;
    }
}

static int addr(AST *e) {
    switch (e->kind) {
        case N_COMPOUND_LIT:
            return compound_lit_addr(e);

        case N_IDENT: {
            Symbol *s = e->sym;
            if (!s) {
                return iconst(IR_I64, 0);
            }
            if (codegen_sym_is_local(s)) {
                int a;
                if (local_vreg(s) >= 0) {
                    diag_error(e->loc, "internal error: the address of a register local");
                    return iconst(IR_I64, 0);
                }
                if (s->captured && s->owner_fn && s->owner_fn != cur_fsym) {
                    a = ir_binop_imm(F, IR_ADD, IR_I64, frame_of(s->owner_fn), s->offset);
                } else {
                    a = addr_local(s->offset);
                }
                if (s->by_ref) {
                    a = ir_load(F, IR_I64, a, 8, 0);
                }
                return a;
            }
            if (s->kind == SYM_FUNCTION && s->parent_fn) {
                /* a nested function's address: its trampoline's, in its parent's frame */
                if (!s->tramp_off) {
                    diag_error(e->loc, "the address of the nested function '%s' cannot be taken on this target", s->name);
                    return iconst(IR_I64, 0);
                }
                return ir_binop_imm(F, IR_ADD, IR_I64, frame_of(s->parent_fn), s->tramp_off);
            }
            return addr_global(s);
        }

        case N_STRING:
            return addr_string(codegen_add_string(e));

        case N_UNARY:
            if (e->u.unary.op == OP_DEREF) {
                return expr(e->u.unary.operand);
            }
            if ((e->u.unary.op == OP_REAL || e->u.unary.op == OP_IMAG) && type_is_complex(e->u.unary.operand->type)) {
                int a = addr(e->u.unary.operand);
                if (e->u.unary.op == OP_IMAG) {
                    a = ir_binop_imm(F, IR_ADD, IR_I64, a, (long)type_size(type_complex_base(e->u.unary.operand->type)));
                }
                return a;
            }
            break;

        case N_MEMBER: {
            /* a struct valued expression evaluates to its address either way */
            int a = expr(e->u.member.object);
            if (e->u.member.member && e->u.member.member->rt_slot) {
                /* after a variable length member: an offset of the run time layout */
                a = ir_binop(F, IR_ADD, IR_I64, a, ir_load(F, IR_I64, addr_local(e->u.member.member->rt_slot), 8, 0));
            } else if (e->u.member.member && e->u.member.member->offset) {
                a = ir_binop_imm(F, IR_ADD, IR_I64, a, (long)e->u.member.member->offset);
            }
            return a;
        }

        case N_INDEX:
            return index_addr(e);

        default:
            break;
    }
    if (e->type && (by_address(e->type) || e->type->kind == TY_ARRAY)) {
        return expr(e);
    }
    diag_error(e->loc, "internal error: %s is not an lvalue", node_kind_name(e->kind));
    return iconst(IR_I64, 0);
}

/* ------------------------------------------------------------------ */
/* conditions                                                         */
/* ------------------------------------------------------------------ */

static IRCond cond_of(OpKind op, int is_unsigned) {
    switch (op) {
        case OP_LT: return is_unsigned ? IR_ULT : IR_LT;
        case OP_GT: return is_unsigned ? IR_UGT : IR_GT;
        case OP_LE: return is_unsigned ? IR_ULE : IR_LE;
        case OP_GE: return is_unsigned ? IR_UGE : IR_GE;
        case OP_EQ: return IR_EQ;
        default: return IR_NE;
    }
}

static int is_comparison(OpKind op) {
    return op == OP_LT || op == OP_GT || op == OP_LE || op == OP_GE || op == OP_EQ || op == OP_NE;
}

static void complex_truth_branch(AST *e, int bt, int bf);
static int complex_truth(AST *e);

/* does `e` already evaluate to 0 or 1: a comparison, !, && or ||, or a _Bool */
static int is_boolean(const AST *e) {
    if (e->kind == N_BINARY) {
        OpKind op = e->u.binary.op;
        return is_comparison(op) || op == OP_AND || op == OP_OR;
    }
    if (e->kind == N_UNARY && e->u.unary.op == OP_NOT) {
        return 1;
    }
    return e->type && e->type->kind == TY_BOOL;
}

/* evaluate `e` as a condition: e != 0, as an i32 that is 0 or 1 */
static int cond_value(AST *e) {
    int v;
    if (type_is_complex(e->type)) {
        return complex_truth(e);
    }
    v = expr(e);
    if (!lower_check_value_type(e->loc, e->type)) {
        return iconst(IR_I32, 0);
    }
    if (is_boolean(e)) {
        return v;
    }
    return truth(v, e->type);
}

static Symbol *direct_callee(AST *f);

/* a cast that leaves the truth of its operand alone: to _Bool from any scalar, or to an integer at least as wide as an integer or pointer operand, or of a 0 or 1 value */
static int keeps_truth(const AST *cast) {
    const AST *inner = cast->u.cast.expr;
    if (!inner->type || !cast->type || type_is_complex(inner->type)) {
        return 0;
    }
    if (cast->type->kind == TY_BOOL) {
        return type_is_scalar(inner->type);
    }
    if (!type_is_integer(cast->type)) {
        return 0;
    }
    if (is_boolean(inner)) {
        return 1;
    }
    return (type_is_integer(inner->type) || type_is_pointer(inner->type)) && type_size(inner->type) <= type_size(cast->type);
}

/* branch on `e`: to `bt` when it is nonzero, else to `bf` */
static void branch(AST *e, int bt, int bf) {
    IRType it;
    int v;

    for (;;) {
        if ((e->kind == N_IMPLICIT_CAST || e->kind == N_CAST) && keeps_truth(e)) {
            e = e->u.cast.expr;     /* the truth test is the same */
        } else if (e->kind == N_CALL && e->u.call.args->len == 2 && direct_callee(e->u.call.func)
                && strcmp(direct_callee(e->u.call.func)->name, "__builtin_expect") == 0) {
            /* the expected value is advice this compiler does not take, though it is still evaluated */
            expr(e->u.call.args->items[1]);
            e = e->u.call.args->items[0];
        } else {
            break;
        }
    }
    if (e->kind == N_UNARY && e->u.unary.op == OP_NOT) {
        branch(e->u.unary.operand, bf, bt);
        return;
    }
    if (e->kind == N_BINARY && (e->u.binary.op == OP_AND || e->u.binary.op == OP_OR)) {
        int mid = ir_block(F);
        if (e->u.binary.op == OP_AND) {
            branch(e->u.binary.left, mid, bf);
        } else {
            branch(e->u.binary.left, bt, mid);
        }
        ir_place(F, mid);
        branch(e->u.binary.right, bt, bf);
        return;
    }
    if (type_is_complex(e->type)) {
        complex_truth_branch(e, bt, bf);
        return;
    }
    v = expr(e);
    if (!lower_check_value_type(e->loc, e->type)) {
        ir_br(F, bf);
        return;
    }
    it = ty(e->type);
    if (it != IR_I32 && it != IR_I64) {
        v = truth(v, e->type);
        it = IR_I32;
    }
    {
        IRInst *i = ir_inst(F, ir_emit(F, IR_CONDBR, it, -1, v, -1));
        i->imm = bt;
        i->aux = bf;
    }
}

/* ------------------------------------------------------------------ */
/* expressions                                                        */
/* ------------------------------------------------------------------ */

static IROp arith_op(OpKind op, int is_unsigned) {
    switch (op) {
        case OP_ADD: case OP_ADD_ASSIGN: return IR_ADD;
        case OP_SUB: case OP_SUB_ASSIGN: return IR_SUB;
        case OP_MUL: case OP_MUL_ASSIGN: return IR_MUL;
        case OP_DIV: case OP_DIV_ASSIGN: return is_unsigned ? IR_UDIV : IR_SDIV;
        case OP_MOD: case OP_MOD_ASSIGN: return is_unsigned ? IR_UREM : IR_SREM;
        case OP_SHL: case OP_SHL_ASSIGN: return IR_SHL;
        case OP_SHR: case OP_SHR_ASSIGN: return is_unsigned ? IR_LSHR : IR_ASHR;
        case OP_BITAND: case OP_AND_ASSIGN: return IR_AND;
        case OP_BITXOR: case OP_XOR_ASSIGN: return IR_XOR;
        case OP_BITOR: case OP_OR_ASSIGN: return IR_OR;
        default: return IR_ADD;
    }
}

static IROp float_op(OpKind op) {
    switch (op) {
        case OP_ADD: case OP_ADD_ASSIGN: return IR_FADD;
        case OP_SUB: case OP_SUB_ASSIGN: return IR_FSUB;
        case OP_MUL: case OP_MUL_ASSIGN: return IR_FMUL;
        default: return IR_FDIV;
    }
}

/* a op b in the arithmetic type `t` (integer or floating), or a comparison yielding an i32 */
/* the operation just emitted, defining v, is signed int arithmetic: marked, for the optimizer (IRF_NSW) */
static int nsw(int v, OpKind op, const Type *t) {
    if ((op == OP_ADD || op == OP_SUB || op == OP_MUL) && type_is_integer(t) && !type_is_unsigned(t) && ty(t) == IR_I32
            && t->kind != TY_BOOL) {
        ir_inst(F, ir_last(F))->flags |= IRF_NSW;
    }
    return v;
}

static int arith(OpKind op, Type *t, int a, int b) {
    IRType it = ty(t);
    if (type_is_floating(t)) {
        if (is_comparison(op)) {
            int dst = ir_vreg(F, IR_I32);
            IRInst *i = ir_inst(F, ir_emit(F, IR_FCMP, it, dst, a, b));
            i->cc = (unsigned char)cond_of(op, 0);
            return dst;
        }
        return ir_binop(F, float_op(op), it, a, b);
    }
    if (is_comparison(op)) {
        return ir_icmp(F, cond_of(op, op_unsigned(t)), it, a, b);
    }
    return nsw(ir_binop(F, arith_op(op, op_unsigned(t)), it, a, b), op, t);
}

/* the same with a constant right operand */
static int arith_imm(OpKind op, Type *t, int a, long imm) {
    IRType it = ty(t);
    if (it == IR_I128) {
        return arith(op, t, a, iconst(IR_I128, imm));
    }
    if (is_comparison(op)) {
        return ir_icmp_imm(F, cond_of(op, op_unsigned(t)), it, a, imm);
    }
    return nsw(ir_binop_imm(F, arith_op(op, op_unsigned(t)), it, a, imm), op, t);
}

static int complex_cast(AST *e);
static int complex_unary(AST *e);
static int complex_binary(AST *e);
static int complex_compound(AST *e);
static int complex_const(AST *e);

static int emit_call(const char *name, int target, int nargs, const int *args, Type **types, int *by_addr,
                     size_t nfixed, int variadic, Type *ret, int sret_addr, int result_addr);

static Loc no_loc_vec(void) {
    Loc l;
    memset(&l, 0, sizeof(l));
    return l;
}

/* ------------------------------------------------------------------ */
/* _Float128 and the decimal types (x86_64): values in memory, the      */
/* arithmetic the compiler runtime's                                    */
/* ------------------------------------------------------------------ */

/*
 * A value of one of these is the address of its bytes (as a record's
 * is): each result goes to a fresh frame slot, and every operation is a
 * call to libgcc: its IEEE binary128 routines (__addtf3, ...) or its
 * BID decimal ones (__bid_adddd3, ...), whose names follow one pattern.
 */
static int is_soft(const Type *t) {
    return t->kind == TY_FLOAT128 || type_is_decimal(t);
}

/* the mode letters of the routines' names: tf, sd, dd, td */
static const char *soft_mode(const Type *t) {
    switch (t->kind) {
        case TY_DECIMAL32: return "sd";
        case TY_DECIMAL64: return "dd";
        case TY_DECIMAL128: return "td";
        default: return "tf";
    }
}

static const char *soft_prefix(const Type *t) {
    return type_is_decimal(t) ? "__bid_" : "__";
}

static int soft_temp(const Type *t) {
    return addr_local(alloc_slot(type_size(t), type_size(t) > 8 ? 16 : 8));
}

/* name(args), a result of the soft type st in a new slot: its address */
static int soft_call_to(const char *name, int n, int *vals, Type **types, Type *st) {
    int by_addr[2];
    int k;
    int res = soft_temp(st);
    for (k = 0; k < n; k++) {
        by_addr[k] = is_soft(types[k]);
    }
    emit_call(ir_name(F, name), -1, n, vals, types, by_addr, (size_t)n, 0, st, -1, res);
    return res;
}

/* name(args) with a scalar result of type ret */
static int soft_call_from(const char *name, int n, int *vals, Type **types, Type *ret) {
    int by_addr[2];
    int k;
    for (k = 0; k < n; k++) {
        by_addr[k] = is_soft(types[k]);
    }
    return emit_call(ir_name(F, name), -1, n, vals, types, by_addr, (size_t)n, 0, ret, -1, -1);
}

/* the routine converting between a binary float of kind `bin` and the decimal type d (to_dec: into d) */
static const char *dec_float_name(const Type *d, TypeKind bin, int to_dec) {
    static const char *const names[3][3][2] = {
        /* sf, df, xf; each [to decimal, from decimal] */
        { { "__bid_extendsfsd", "__bid_truncsdsf" }, { "__bid_truncdfsd", "__bid_extendsddf" }, { "__bid_truncxfsd", "__bid_extendsdxf" } },
        { { "__bid_extendsfdd", "__bid_truncddsf" }, { "__bid_extenddfdd", "__bid_truncdddf" }, { "__bid_truncxfdd", "__bid_extendddxf" } },
        { { "__bid_extendsftd", "__bid_trunctdsf" }, { "__bid_extenddftd", "__bid_trunctddf" }, { "__bid_extendxftd", "__bid_trunctdxf" } }
    };
    int di = d->kind == TY_DECIMAL32 ? 0 : d->kind == TY_DECIMAL64 ? 1 : 2;
    int bi = bin == TY_FLOAT ? 0 : bin == TY_DOUBLE ? 1 : 2;
    return names[di][bi][to_dec ? 0 : 1];
}

/* the scalar (or other soft value) v of type `from` as a value of the soft type st */
static int soft_from(int v, Type *from, Type *st) {
    char name[48];
    Type *t = from;
    const char *m = soft_mode(st);
    const char *pre = soft_prefix(st);
    if (from->kind == st->kind) {
        return v;
    }
    if (is_soft(from)) {
        if (from->kind == TY_FLOAT128 || st->kind == TY_FLOAT128) {
            diag_error(no_loc_vec(), "conversion between _Float128 and a decimal type is not supported");
            return soft_temp(st);
        }
        /* between decimal types */
        snprintf(name, sizeof(name), "__bid_%s%s%s2", from->kind < st->kind ? "extend" : "trunc", soft_mode(from), m);
        return soft_call_to(name, 1, &v, &t, st);
    }
    if (type_is_floating(from)) {
        TypeKind bin = from->kind == TY_FLOAT ? TY_FLOAT
                       : from->kind == TY_DOUBLE || type_size(from) == 8 ? TY_DOUBLE : TY_LDOUBLE;
        if (bin == TY_DOUBLE && from->kind != TY_DOUBLE) {
            t = type_basic(TY_DOUBLE);
            v = convert(v, from, t);
        }
        if (st->kind == TY_FLOAT128) {
            snprintf(name, sizeof(name), "__extend%stf2", bin == TY_FLOAT ? "sf" : bin == TY_DOUBLE ? "df" : "xf");
        } else {
            snprintf(name, sizeof(name), "%s", dec_float_name(st, bin, 1));
        }
        return soft_call_to(name, 1, &v, &t, st);
    }
    /* integers: an int's or unsigned int's (every narrower one's value is one), a 64 bit or a 128 bit one's */
    {
        int u = type_is_unsigned(from) || from->kind == TY_POINTER;
        const char *im;
        if (type_is_wide(from)) {
            im = "ti";
        } else if (type_size(from) == 8 || from->kind == TY_POINTER) {
            im = "di";
            t = type_basic(u ? TY_ULONG : TY_LONG);
            v = convert(v, from, t);
        } else {
            u = from->kind == TY_UINT;
            im = "si";
            t = type_basic(u ? TY_UINT : TY_INT);
            v = convert(v, from, t);
        }
        snprintf(name, sizeof(name), "%sfloat%s%s%s", pre, u ? (st->kind == TY_FLOAT128 ? "un" : "uns") : "", im, m);
        return soft_call_to(name, 1, &v, &t, st);
    }
}

/* the soft value at a, of type st, as a value of type `to` */
static int soft_to(int a, Type *st, Type *to) {
    char name[48];
    Type *t;
    const char *m = soft_mode(st);
    const char *pre = soft_prefix(st);
    if (to->kind == st->kind) {
        return a;
    }
    if (is_soft(to)) {
        return soft_from(a, st, to);
    }
    if (to->kind == TY_BOOL) {
        return truth(a, st);
    }
    if (type_is_floating(to)) {
        TypeKind bin = to->kind == TY_FLOAT ? TY_FLOAT : to->kind == TY_DOUBLE || type_size(to) == 8 ? TY_DOUBLE : TY_LDOUBLE;
        Type *rt = type_basic(bin);
        if (st->kind == TY_FLOAT128) {
            snprintf(name, sizeof(name), "__trunctf%s2", bin == TY_FLOAT ? "sf" : bin == TY_DOUBLE ? "df" : "xf");
        } else {
            snprintf(name, sizeof(name), "%s", dec_float_name(st, bin, 0));
        }
        return convert(soft_call_from(name, 1, &a, &st, rt), rt, to);
    }
    {
        int u = type_is_unsigned(to) || to->kind == TY_POINTER;
        const char *im;
        if (type_is_wide(to)) {
            im = "ti";
            t = to;
        } else if (type_size(to) == 8 || to->kind == TY_POINTER) {
            im = "di";
            t = type_basic(u ? TY_ULONG : TY_LONG);
        } else {
            im = "si";
            t = type_basic(u ? TY_UINT : TY_INT);
        }
        snprintf(name, sizeof(name), st->kind == TY_FLOAT128 ? "%sfix%s%s%s" : "%sfix%s%s%s", pre, u ? "uns" : "",
                 st->kind == TY_FLOAT128 ? m : m, im);
        return convert(soft_call_from(name, 1, &a, &st, t), t, to);
    }
}

/* a op b, both of the soft type st: the result's address, or for a comparison the int */
static int soft_arith(OpKind op, Type *st, int a, int b) {
    char name[48];
    int vals[2];
    Type *types[2];
    const char *m = soft_mode(st);
    const char *pre = soft_prefix(st);
    const char *what;
    IRCond cc;
    int r;
    vals[0] = a;
    vals[1] = b;
    types[0] = types[1] = st;
    switch (op) {
        case OP_ADD: case OP_ADD_ASSIGN: what = "add"; break;
        case OP_SUB: case OP_SUB_ASSIGN: what = "sub"; break;
        case OP_MUL: case OP_MUL_ASSIGN: what = "mul"; break;
        case OP_DIV: case OP_DIV_ASSIGN: what = "div"; break;
        case OP_EQ: what = "eq"; cc = IR_EQ; break;
        case OP_NE: what = "ne"; cc = IR_NE; break;
        case OP_LT: what = "lt"; cc = IR_LT; break;
        case OP_LE: what = "le"; cc = IR_LE; break;
        case OP_GT: what = "gt"; cc = IR_GT; break;
        case OP_GE: what = "ge"; cc = IR_GE; break;
        default:
            fatal("internal error: an operator %s does not have", type_kind_name(st->kind));
            return iconst(IR_I32, 0);
    }
    if (op == OP_ADD || op == OP_SUB || op == OP_MUL || op == OP_DIV || op == OP_ADD_ASSIGN || op == OP_SUB_ASSIGN
            || op == OP_MUL_ASSIGN || op == OP_DIV_ASSIGN) {
        snprintf(name, sizeof(name), "%s%s%s3", pre, what, m);
        return soft_call_to(name, 2, vals, types, st);
    }
    /* each returns an int that compares with 0 as the operands do (a NaN makes it false) */
    snprintf(name, sizeof(name), "%s%s%s2", pre, what, m);
    r = soft_call_from(name, 2, vals, types, type_basic(TY_INT));
    return ir_icmp_imm(F, cc, IR_I32, r, 0);
}

/* the value at a negated: the sign bit (its top one, in every one of these encodings) flipped in a copy */
static int soft_neg(int a, Type *st) {
    size_t n = type_size(st);
    int res = soft_temp(st);
    int hi;
    IRType it = n == 4 ? IR_I32 : IR_I64;
    copy_bytes(res, a, n);
    hi = n == 16 ? ir_binop_imm(F, IR_ADD, IR_I64, res, 8) : res;
    ir_store(F, it, hi, ir_binop(F, IR_XOR, it, ir_load(F, it, hi, it == IR_I32 ? 4 : 8, 0),
                                 iconst(it, it == IR_I32 ? (long)0x80000000ul : (long)(1ul << 63))), it == IR_I32 ? 4 : 8);
    return res;
}

static int soft_zero(Type *st) {
    int z = soft_temp(st);
    zero_bytes(z, type_size(st));
    return z;
}

/* ------------------------------------------------------------------ */
/* GNU vectors: values in memory, the operations element by element    */
/* ------------------------------------------------------------------ */

static int vec_temp(const Type *v) {
    return addr_local(alloc_slot(type_size(v), type_align(v) > 16 ? 16 : type_align(v)));
}

/* element k of the operand: of a vector at address a, or the scalar a itself (the same for every element) */
static int vec_elem(int a, Type *t, long k) {
    Type *et;
    if (!type_is_vector(t)) {
        return a;
    }
    et = t->base;
    return load_value(no_loc_vec(), et, k ? ir_binop_imm(F, IR_ADD, IR_I64, a, k * (long)type_size(et)) : a);
}

/* l op r for vectors (or a vector and a scalar) into a new slot of type vt: its address */
static int vec_arith(OpKind op, Type *vt, int la, Type *lt, int ra, Type *rt) {
    Type *ot = type_is_vector(lt) ? lt->base : rt->base;   /* the operands' element type */
    Type *et = vt->base;
    Type *pt = type_promote(ot);
    int res = vec_temp(vt);
    long k;
    OpKind base = op;
    switch (op) {
        case OP_ADD_ASSIGN: base = OP_ADD; break;
        case OP_SUB_ASSIGN: base = OP_SUB; break;
        case OP_MUL_ASSIGN: base = OP_MUL; break;
        case OP_DIV_ASSIGN: base = OP_DIV; break;
        case OP_MOD_ASSIGN: base = OP_MOD; break;
        case OP_AND_ASSIGN: base = OP_BITAND; break;
        case OP_OR_ASSIGN: base = OP_BITOR; break;
        case OP_XOR_ASSIGN: base = OP_BITXOR; break;
        case OP_SHL_ASSIGN: base = OP_SHL; break;
        case OP_SHR_ASSIGN: base = OP_SHR; break;
        default: break;
    }
    for (k = 0; k < vt->length; k++) {
        Type *lte = type_is_vector(lt) ? lt->base : lt;
        Type *rte = type_is_vector(rt) ? rt->base : rt;
        int lv = convert(vec_elem(la, lt, k), lte, pt);
        int rv = convert(vec_elem(ra, rt, k), rte, (base == OP_SHL || base == OP_SHR) ? pt : pt);
        int v = arith(base, pt, lv, rv);
        if (is_comparison(base)) {
            v = convert(ir_unop(F, IR_NEG, IR_I32, v), type_basic(TY_INT), et);     /* true is all ones */
        } else {
            v = convert(canon(v, pt), pt, et);
        }
        store_value(et, k ? ir_binop_imm(F, IR_ADD, IR_I64, res, k * (long)type_size(et)) : res, v);
    }
    return res;
}

static int vec_unary(OpKind op, Type *vt, int a) {
    Type *et = vt->base;
    Type *pt = type_promote(et);
    int res = vec_temp(vt);
    long k;
    for (k = 0; k < vt->length; k++) {
        int v = convert(vec_elem(a, vt, k), et, pt);
        if (op == OP_NEG) {
            v = type_is_floating(pt) ? ir_unop(F, IR_FNEG, ty(pt), v) : ir_unop(F, IR_NEG, ty(pt), v);
        } else if (op == OP_BITNOT) {
            v = ir_unop(F, IR_NOT, ty(pt), v);
        }
        store_value(et, k ? ir_binop_imm(F, IR_ADD, IR_I64, res, k * (long)type_size(et)) : res, convert(canon(v, pt), pt, et));
    }
    return res;
}

/* a cast to or from a vector: the bits as they are, between it and another vector or an integer of its size */
static int vec_cast(int v, Type *from, Type *to) {
    int res;
    if (type_is_vector(from) && type_is_vector(to)) {
        res = vec_temp(to);
        copy_bytes(res, v, type_size(to));
        return res;
    }
    if (type_is_vector(from)) {
        return load_value(no_loc_vec(), type_unqualified(to), v);
    }
    res = vec_temp(to);
    store_value(type_unqualified(from), res, v);
    return res;
}

static int cast(AST *e) {
    Type *to = e->type;
    AST *x = e->u.cast.expr;
    Type *from = x->type;

    if (e->kind == N_CAST && e->u.cast.type && type_is_vm(e->u.cast.type->type)) {
        vla_eval(e->u.cast.type->u.typed.declarator);    /* `(int (*)[n])p` */
    }

    if (to->kind == TY_VOID) {
        expr(x);
        return -1;
    }
    if (type_is_vector(to) || type_is_vector(from)) {
        return vec_cast(expr(x), from, to);
    }
    if (from->kind == TY_ARRAY || from->kind == TY_FUNC || (from->kind == TY_VA_LIST && to->kind == TY_POINTER)) {
        return addr(x);
    }
    if (type_is_record(to) && type_is_record(from)) {
        return expr(x);     /* the wrapper __auto_type puts on its initializer: the same record */
    }
    if (type_is_complex(to) || type_is_complex(from)) {
        return complex_cast(e);
    }
    if (type_is_vector(to) || type_is_vector(from)) {
        return vec_cast(expr(x), from, to);
    }
    if (is_soft(to) || is_soft(from)) {
        if (!lower_check_value_type(e->loc, to) || !lower_check_value_type(x->loc, from)) {
            return iconst(IR_I64, 0);
        }
        return is_soft(to) ? soft_from(expr(x), from, to) : soft_to(expr(x), from, to);
    }
    if (!lower_check_value_type(e->loc, to) || !lower_check_value_type(x->loc, from)) {
        return iconst(ty(to), 0);
    }
    return convert(expr(x), from, to);
}

/* old + 1 or - 1, in the object's type (delta: a pointer's element size) */
static int incdec_value(Type *t, int old, int sign, long delta) {
    int nw;
    if (t->kind == TY_POINTER && type_is_vla(t->base)) {
        nw = ir_binop(F, sign > 0 ? IR_ADD : IR_SUB, IR_I64, old, size_of(t->base));
    } else if (type_is_floating(t)) {
        nw = ir_binop(F, IR_FADD, ty(t), old, float_const((long double)sign, t));
    } else if (t->kind == TY_BOOL) {
        /* a _Bool: ++ sets it, -- flips it (old + step, then != 0) */
        nw = ir_binop_imm(F, IR_ADD, IR_I32, old, sign);
        nw = ir_icmp_imm(F, IR_NE, IR_I32, nw, 0);
    } else if (type_is_wide(t)) {
        nw = ir_binop(F, IR_ADD, IR_I128, old, iconst(IR_I128, sign * delta));
    } else {
        nw = ir_binop_imm(F, IR_ADD, ty(t), old, sign * delta);
        if (t->kind != TY_POINTER && type_size(t) == 4) {
            nsw(nw, OP_ADD, t);
        }
        nw = canon(nw, t);
    }
    return nw;
}

/* ++ and --: the object is updated in place, `post` leaves the old value */
static int incdec(AST *e, int sign, int post) {
    AST *o = e->u.unary.operand;
    Type *t = o->type;
    long delta = 1;
    int a;
    int old;
    int nw;

    if (!lower_check_value_type(e->loc, t)) {
        return iconst(IR_I32, 0);
    }
    if (t->kind == TY_POINTER) {
        delta = (long)type_size(t->base);
    }
    if (is_soft(t)) {
        Type *st = type_unqualified(t);
        int one = soft_from(iconst(IR_I32, 1), type_basic(TY_INT), st);
        int res, oldc = -1;
        a = lvalue_addr(o);
        if (post) {
            oldc = soft_temp(st);
            copy_bytes(oldc, a, type_size(st));
        }
        res = soft_arith(sign > 0 ? OP_ADD : OP_SUB, st, a, one);
        copy_bytes(a, res, type_size(st));
        return post ? oldc : a;
    }
    a = lvalue_addr(o);
    if (a >= 0 && atomic_object(t) && !sema_is_bitfield(o)) {
        AtomicLoop lp;
        atomic_loop_begin(&lp, a, t);
        nw = incdec_value(t, lp.cur, sign, delta);
        atomic_loop_end(&lp, nw);
        /* lp.cur is now the value replaced, the old one */
        return post ? lp.cur : nw;
    }
    old = lvalue_load(o, a);
    nw = incdec_value(t, old, sign, delta);
    if (a < 0 && post) {
        /* the register is about to change: keep the old value */
        int keep = ir_vreg(F, ty(t));
        ir_mov(F, keep, old);
        old = keep;
    }
    nw = lvalue_store(o, a, nw);
    return post ? old : nw;
}

static int unary(AST *e) {
    AST *o = e->u.unary.operand;
    int v;

    switch (e->u.unary.op) {
        case OP_ADDR:
            return addr(o);

        case OP_DEREF:
            v = expr(o);
            if (e->type->kind == TY_FUNC) {
                return v;
            }
            return load_value(e->loc, e->type, v);

        case OP_POS:
            return expr(o);

        case OP_REAL:
        case OP_IMAG:
            if (type_is_complex(o->type)) {
                return load_value(e->loc, e->type, addr(e));
            }
            v = expr(o);
            if (e->u.unary.op == OP_REAL) {
                return v;
            }
            /* the imaginary part of a real: 0 of its type, after the operand's side effects */
            if (type_is_floating(e->type)) {
                return float_const(0, e->type);
            }
            return iconst(ty(e->type), 0);

        case OP_NEG:
            if (type_is_complex(e->type)) {
                return complex_unary(e);
            }
            if (is_soft(e->type)) {
                return soft_neg(expr(o), e->type);
            }
            if (type_is_vector(e->type)) {
                return vec_unary(OP_NEG, e->type, expr(o));
            }
            v = expr(o);
            if (!lower_check_value_type(e->loc, e->type)) {
                return v;
            }
            if (type_is_floating(e->type)) {
                return ir_unop(F, IR_FNEG, ty(e->type), v);
            }
            return canon(ir_unop(F, IR_NEG, ty(e->type), v), e->type);

        case OP_BITNOT:
            if (type_is_complex(e->type)) {
                return complex_unary(e);
            }
            if (type_is_vector(e->type)) {
                return vec_unary(OP_BITNOT, e->type, expr(o));
            }
            v = expr(o);
            return canon(ir_unop(F, IR_NOT, ty(e->type), v), e->type);

        case OP_NOT:
            return ir_icmp_imm(F, IR_EQ, IR_I32, cond_value(o), 0);

        case OP_PREINC: return incdec(e, 1, 0);
        case OP_PREDEC: return incdec(e, -1, 0);
        case OP_POSTINC: return incdec(e, 1, 1);
        case OP_POSTDEC: return incdec(e, -1, 1);

        default:
            diag_error(e->loc, "internal error: unknown unary operator");
            return iconst(IR_I32, 0);
    }
}

static int binary(AST *e) {
    OpKind op = e->u.binary.op;
    AST *l = e->u.binary.left;
    AST *r = e->u.binary.right;
    long cv;
    int lv;
    int rv;

    if (op == OP_AND || op == OP_OR) {
        int bt = ir_block(F);
        int bf = ir_block(F);
        int join = ir_block(F);
        int dst = ir_vreg(F, IR_I32);
        branch(e, bt, bf);
        ir_place(F, bt);
        ir_mov(F, dst, iconst(IR_I32, 1));
        ir_br(F, join);
        ir_place(F, bf);
        ir_mov(F, dst, iconst(IR_I32, 0));
        ir_place(F, join);
        return dst;
    }
    if (op == OP_COMMA) {
        expr(l);
        return expr(r);
    }
    if (type_is_complex(l->type)) {
        return complex_binary(e);
    }
    if (is_soft(l->type) && is_soft(r->type)) {
        int la = expr(l);
        return soft_arith(op, type_unqualified(l->type), la, expr(r));
    }
    if (type_is_vector(e->type) || type_is_vector(l->type) || type_is_vector(r->type)) {
        int la = expr(l);
        return vec_arith(op, e->type, la, l->type, expr(r), r->type);
    }
    if (!lower_check_value_type(l->loc, l->type) || !lower_check_value_type(r->loc, r->type)) {
        return iconst(ty(e->type), 0);
    }
    if (op == OP_ADD && type_is_pointer(r->type) && !type_is_pointer(l->type)) {
        /* integer + pointer */
        AST *t = l;
        l = r;
        r = t;
    }
    if ((op == OP_ADD || op == OP_SUB) && type_is_pointer(l->type) && !type_is_pointer(r->type)) {
        size_t size = type_size(l->type->base);
        lv = expr(l);
        if (type_is_vla(l->type->base)) {
            rv = scale_by(scaled_index(expr(r), r->type, 1), l->type->base);
            return ir_binop(F, op == OP_ADD ? IR_ADD : IR_SUB, IR_I64, lv, rv);
        }
        if (sema_const_eval(r, &cv)) {
            long off = (long)((unsigned long)cv * size);
            if (off == 0) {
                return lv;
            }
            return ir_binop_imm(F, op == OP_ADD ? IR_ADD : IR_SUB, IR_I64, lv, off);
        }
        rv = scaled_index(expr(r), r->type, size);
        return ir_binop(F, op == OP_ADD ? IR_ADD : IR_SUB, IR_I64, lv, rv);
    }
    if (op == OP_SUB && type_is_pointer(l->type) && type_is_pointer(r->type)) {
        /* the byte difference divided by the element size */
        size_t size = type_size(l->type->base);
        int d;
        lv = expr(l);
        rv = expr(r);
        d = ir_binop(F, IR_SUB, IR_I64, lv, rv);
        if (type_is_vla(l->type->base)) {
            return ir_binop(F, IR_SDIV, IR_I64, d, size_of(l->type->base));
        }
        if (size != 1) {
            d = ir_binop_imm(F, IR_SDIV, IR_I64, d, (long)size);
        }
        return d;
    }
    lv = expr(l);
    if (!type_is_floating(l->type) && !type_is_wide(l->type) && sema_const_eval(r, &cv)) {
        rv = arith_imm(op, l->type, lv, sema_fit(cv, r->type));
    } else {
        rv = expr(r);
        if (type_is_wide(l->type) && (op == OP_SHL || op == OP_SHR)) {
            /* the count has its own type; the instruction takes an i64 */
            rv = convert(rv, r->type, type_basic(TY_LONG));
        }
        rv = arith(op, l->type, lv, rv);
    }
    if (!is_comparison(op)) {
        rv = canon(rv, e->type);
    }
    return rv;
}

static int atomic_compound(AST *e, int a, Type *common) {
    OpKind op = e->u.binary.op;
    AST *r = e->u.binary.right;
    Type *t = e->u.binary.left->type;
    AtomicLoop lp;
    int rv, nw;
    if (t->kind == TY_POINTER) {
        rv = type_is_vla(t->base) ? scale_by(scaled_index(expr(r), r->type, 1), t->base)
             : scaled_index(expr(r), r->type, type_size(t->base));
    } else {
        rv = expr(r);
        if (type_is_wide(common) && (op == OP_SHL_ASSIGN || op == OP_SHR_ASSIGN)) {
            rv = convert(rv, r->type, type_basic(TY_LONG));
        }
    }
    atomic_loop_begin(&lp, a, t);
    if (t->kind == TY_POINTER) {
        nw = ir_binop(F, op == OP_ADD_ASSIGN ? IR_ADD : IR_SUB, IR_I64, lp.cur, rv);
    } else {
        nw = convert(arith(op, common, convert(lp.cur, t, common), rv), common, t);
    }
    atomic_loop_end(&lp, nw);
    return nw;
}

static int assign(AST *e) {
    OpKind op = e->u.binary.op;
    AST *l = e->u.binary.left;
    AST *r = e->u.binary.right;
    Type *t = l->type;
    Type *common;
    int a;
    int lv;
    int rv;
    long cv;

    if (op == OP_ASSIGN) {
        if (by_address(t)) {
            a = addr(l);
            rv = expr(r);
            copy_object(a, rv, t);
            return a;
        }
        if (!lower_check_value_type(e->loc, t)) {
            return iconst(ty(t), 0);
        }
        a = lvalue_addr(l);
        rv = expr(r);
        return lvalue_store(l, a, rv);
    }
    if (type_is_complex(t)) {
        return complex_compound(e);
    }
    if (type_is_vector(t) && op != OP_ASSIGN) {
        int rv2 = expr(r);
        int la = addr(l);
        copy_bytes(la, vec_arith(op, type_unqualified(t), la, t, rv2, r->type), type_size(t));
        return la;
    }
    if ((is_soft(t) || is_soft(r->type)) && op != OP_ASSIGN) {
        /* in the soft type (sema converted the right operand to it), then back to the object's type */
        Type *st = is_soft(r->type) ? type_unqualified(r->type) : type_unqualified(t);
        int rv2 = expr(r);
        int la = lvalue_addr(l);
        int res = soft_arith(op, st, soft_from(lvalue_load(l, la), t, st), soft_from(rv2, r->type, st));
        if (is_soft(t)) {
            res = soft_from(res, st, type_unqualified(t));
            copy_bytes(la, res, type_size(t));
            return la;
        }
        return lvalue_store(l, la, soft_to(res, st, t));
    }
    if (type_is_complex(r->type)) {
        unsupported(e->loc, "compound assignment of a complex value to a real object");
        return iconst(IR_I32, 0);
    }
    if (!lower_check_value_type(e->loc, t) || !lower_check_value_type(r->loc, r->type)) {
        return iconst(ty(t), 0);
    }
    /*
     * The operation happens in the common type that sema converted the
     * right operand to (or the promoted left type for shifts), then the
     * result is converted back to the left operand's type.
     */
    if (t->kind == TY_POINTER) {
        common = t;
    } else if (op == OP_SHL_ASSIGN || op == OP_SHR_ASSIGN) {
        common = type_promote(t);
    } else {
        common = r->type;
    }
    a = lvalue_addr(l);
    if (a >= 0 && atomic_object(t) && !sema_is_bitfield(l)) {
        return atomic_compound(e, a, common);
    }
    if (t->kind == TY_POINTER) {
        size_t size = type_size(t->base);
        lv = lvalue_load(l, a);
        if (type_is_vla(t->base)) {
            rv = scale_by(scaled_index(expr(r), r->type, 1), t->base);
            rv = ir_binop(F, op == OP_ADD_ASSIGN ? IR_ADD : IR_SUB, IR_I64, lv, rv);
        } else if (sema_const_eval(r, &cv)) {
            rv = ir_binop_imm(F, op == OP_ADD_ASSIGN ? IR_ADD : IR_SUB, IR_I64, lv, (long)((unsigned long)cv * size));
        } else {
            rv = scaled_index(expr(r), r->type, size);
            rv = ir_binop(F, op == OP_ADD_ASSIGN ? IR_ADD : IR_SUB, IR_I64, lv, rv);
        }
        return lvalue_store(l, a, rv);
    }
    if (!type_is_floating(common) && !type_is_wide(common) && sema_const_eval(r, &cv)) {
        lv = convert(lvalue_load(l, a), t, common);
        rv = arith_imm(op, common, lv, sema_fit(cv, r->type));
    } else {
        rv = expr(r);
        lv = convert(lvalue_load(l, a), t, common);
        if (type_is_wide(common) && (op == OP_SHL_ASSIGN || op == OP_SHR_ASSIGN)) {
            rv = convert(rv, r->type, type_basic(TY_LONG));
        }
        rv = arith(op, common, lv, rv);
    }
    rv = convert(rv, common, t);
    return lvalue_store(l, a, rv);
}

static int conditional(AST *e) {
    int bt = ir_block(F);
    int bf = ir_block(F);
    int join = ir_block(F);
    int dst = -1;
    int v;

    branch(e->u.cond.cond, bt, bf);
    ir_place(F, bt);
    v = expr(e->u.cond.then);
    if (v >= 0) {
        dst = ir_vreg(F, (IRType)F->vregs[v].type);
        ir_mov(F, dst, v);
    }
    ir_br(F, join);
    ir_place(F, bf);
    v = expr(e->u.cond.otherwise);
    if (v >= 0 && dst >= 0) {
        ir_mov(F, dst, v);
    }
    ir_place(F, join);
    return dst;
}

/* the function a call names directly, or NULL for a call through a pointer value */
static Symbol *direct_callee(AST *f) {
    while (f->kind == N_IMPLICIT_CAST || f->kind == N_CAST) {
        f = f->u.cast.expr;
    }
    if (f->kind == N_UNARY && f->u.unary.op == OP_DEREF) {
        return direct_callee(f->u.unary.operand);
    }
    if (f->kind == N_IDENT && f->sym && f->sym->kind == SYM_FUNCTION) {
        return f->sym;
    }
    return NULL;
}

/*
 * The address of the va_list object `ap` names. Where va_list is an
 * array type the expression has decayed to a pointer to it; otherwise it
 * is the object itself (possibly a parameter), which must not be loaded.
 */
static int va_addr(AST *ap) {
    AST *e = ap;
    while (e->kind == N_IMPLICIT_CAST) {
        e = e->u.cast.expr;
    }
    if (e->type->kind == TY_VA_LIST) {
        return addr(e);
    }
    return expr(ap);
}

static int va_arg_expr(AST *e) {
    Type *t = e->type;
    ArgType at;
    int ap;
    int dst;
    IRInst *i;

    codegen_arg_type(t, &at);
    if (!by_address(t) && !lower_check_value_type(e->loc, t)) {
        return iconst(IR_I32, 0);
    }
    ap = va_addr(e->u.cast.expr);
    dst = ir_vreg(F, IR_I64);
    i = ir_inst(F, ir_emit(F, IR_VAARG, IR_I64, dst, ap, -1));
    i->aux = ir_add_argtype(F, &at);
    i->imm = by_address(t) ? e->temp : 0;
    if (type_is_record(t) && t->record->vm) {
        return ir_load(F, IR_I64, dst, 8, 0);      /* passed by a copy's address */
    }
    if (by_address(t)) {
        return dst;
    }
    return load_value(e->loc, t, dst);
}

/* a call sequence built by hand: `name` with the scalar arguments `args` of types `types`; the result is a record stored at *result_addr or a scalar */
static int emit_call(const char *name, int target, int nargs, const int *args, Type **types, int *by_addr,
                     size_t nfixed, int variadic, Type *ret, int sret_addr, int result_addr) {
    IRCall c;
    IRArg a;
    int k;
    int dst = -1;
    IRInst *i;

    memset(&c, 0, sizeof(c));
    c.name = name;
    c.target = target;
    c.first_arg = F->nargs;
    c.nargs = nargs;
    c.nfixed = nfixed;
    c.variadic = variadic;
    c.sret_addr = sret_addr;
    c.result_addr = result_addr;
    for (k = 0; k < nargs; k++) {
        memset(&a, 0, sizeof(a));
        a.v = args[k];
        a.by_addr = by_addr ? by_addr[k] : 0;
        codegen_arg_type(types[k], &a.type);
        ir_add_arg(F, &a);
    }
    if (result_addr >= 0) {
        codegen_arg_type(ret, &c.ret);
    } else if (ret && !type_is_void(ret) && !by_address(ret)) {
        dst = ir_vreg(F, ty(ret));
    }
    i = ir_inst(F, ir_emit(F, IR_CALL, dst >= 0 ? ty(ret) : IR_VOID, dst, -1, -1));
    i->imm = ir_add_call(F, &c);
    return dst;
}

/* ------------------------------------------------------------------ */
/* atomics: calls to the backend's helpers (AtomicHelper, src/arch.h)    */
/* ------------------------------------------------------------------ */

/* the unsigned integer type of `size` bytes, which the helpers take and return */
static Type *atomic_bits_type(size_t size) {
    return type_basic(size == 1 ? TY_UCHAR : size == 2 ? TY_USHORT : size == 4 ? TY_UINT : TY_ULONG);
}

/* __jcc_atomic_<op>_<size>(p, v, w): the old value, of atomic_bits_type(size) (-1 for store) */
static int atomic_call(const char *op, size_t size, int p, int v, int w) {
    char name[64];
    int vals[3];
    Type *types[3];
    int n = 0;
    Type *bt = atomic_bits_type(size);
    snprintf(name, sizeof(name), "__jcc_atomic_%s_%lu", op, (unsigned long)size);
    codegen_use_atomic(op, (int)size);
    vals[n] = p;
    types[n++] = type_pointer(type_basic(TY_VOID));
    if (v >= 0) {
        vals[n] = v;
        types[n++] = bt;
    }
    if (w >= 0) {
        vals[n] = w;
        types[n++] = bt;
    }
    return emit_call(ir_name(F, name), -1, n, vals, types, NULL, (size_t)n, 0,
                     strcmp(op, "store") == 0 ? type_basic(TY_VOID) : bt, -1, -1);
}

/* a scalar value of type t as the bits the helpers take, and back */
static int atomic_to_bits(int v, Type *t) {
    size_t n = type_size(t);
    if (type_is_floating(t)) {
        int dst = ir_vreg(F, n == 4 ? IR_I32 : IR_I64);
        IRInst *i = ir_inst(F, ir_emit(F, IR_BITCAST, n == 4 ? IR_I32 : IR_I64, dst, v, -1));
        i->t2 = (unsigned char)ty(t);
        return dst;
    }
    return convert(v, t, atomic_bits_type(n));
}

static int atomic_from_bits(int v, Type *t) {
    size_t n = type_size(t);
    if (type_is_floating(t)) {
        int dst = ir_vreg(F, ty(t));
        IRInst *i = ir_inst(F, ir_emit(F, IR_BITCAST, ty(t), dst, v, -1));
        i->t2 = (unsigned char)(n == 4 ? IR_I32 : IR_I64);
        return dst;
    }
    return convert(v, atomic_bits_type(n), t);
}

/* old op v, as the _fetch forms return it */
static int atomic_apply(const char *op, Type *t, int old, int v) {
    IRType it = ty(t);
    int r;
    if (strcmp(op, "nand") == 0) {
        return canon(ir_unop(F, IR_NOT, it, ir_binop(F, IR_AND, it, old, v)), t);
    }
    r = ir_binop(F, strcmp(op, "add") == 0 ? IR_ADD : strcmp(op, "sub") == 0 ? IR_SUB : strcmp(op, "and") == 0 ? IR_AND
                 : strcmp(op, "or") == 0 ? IR_OR : IR_XOR, it, old, v);
    return canon(r, t);
}

static int atomic_builtin(AST *e, const AtomicBuiltin *ab) {
    ASTList *args = e->u.call.args;
    Type *obj;
    size_t n;
    int p, v, w, old, ok;
    Type *bt;
    size_t i;

    if (ab->kind == AK_FENCE) {
        for (i = 0; i < args->len; i++) {
            expr(args->items[i]);
        }
        ir_emit(F, IR_FENCE, IR_VOID, -1, -1, -1);
        return -1;
    }
    if (ab->kind == AK_LOCK_FREE) {
        long size = 0;
        for (i = 1; i < args->len; i++) {
            expr(args->items[i]);
        }
        if (!sema_const_eval(args->items[0], &size)) {
            int s = expr(args->items[0]);
            s = convert(s, args->items[0]->type, type_basic(TY_ULONG));
            /* 1, 2, 4 or 8: a power of two below 16 */
            return ir_binop(F, IR_AND, IR_I32, ir_icmp_imm(F, IR_ULT, IR_I64, s, 9),
                            ir_icmp_imm(F, IR_EQ, IR_I64, ir_binop(F, IR_AND, IR_I64, s, ir_binop_imm(F, IR_SUB, IR_I64, s, 1)), 0));
        }
        return iconst(IR_I32, size == 1 || size == 2 || size == 4 || size == 8);
    }
    obj = type_unqualified(args->items[0]->type->base);
    n = type_size(obj);
    bt = atomic_bits_type(n);
    p = expr(args->items[0]);
    switch (ab->kind) {
        case AK_LOAD_N:
            old = atomic_call("load", n, p, -1, -1);
            for (i = 1; i < args->len; i++) {
                expr(args->items[i]);
            }
            return atomic_from_bits(old, obj);
        case AK_LOAD:
            w = expr(args->items[1]);
            old = atomic_call("load", n, p, -1, -1);
            store_value(bt, w, old);
            return -1;
        case AK_INIT:
            store_value(obj, p, expr(args->items[1]));
            return -1;
        case AK_STORE_N:
            atomic_call("store", n, p, atomic_to_bits(expr(args->items[1]), obj), -1);
            return -1;
        case AK_STORE:
            atomic_call("store", n, p, load_value(e->loc, bt, expr(args->items[1])), -1);
            return -1;
        case AK_RELEASE:
        case AK_CLEAR:
            atomic_call("store", n, p, iconst(ty(bt), 0), -1);
            return -1;
        case AK_TAS:
            old = atomic_call("xchg", n, p, iconst(ty(bt), 1), -1);
            return ir_icmp_imm(F, IR_NE, ty(bt), old, 0);
        case AK_XCHG_N:
            old = atomic_call("xchg", n, p, atomic_to_bits(expr(args->items[1]), obj), -1);
            return atomic_from_bits(old, obj);
        case AK_XCHG:
            v = load_value(e->loc, bt, expr(args->items[1]));
            w = expr(args->items[2]);
            old = atomic_call("xchg", n, p, v, -1);
            store_value(bt, w, old);
            return -1;
        case AK_SYNC_BOOL_CAS:
        case AK_SYNC_VAL_CAS:
            v = atomic_to_bits(expr(args->items[1]), obj);
            w = atomic_to_bits(expr(args->items[2]), obj);
            old = atomic_call("cas", n, p, v, w);
            if (ab->kind == AK_SYNC_BOOL_CAS) {
                return ir_icmp(F, IR_EQ, ty(bt), old, v);
            }
            return atomic_from_bits(old, obj);
        case AK_CAS_N:
        case AK_CAS: {
            /* the old value goes to *expected whatever happens: on success it is what was there */
            int ep = expr(args->items[1]);
            int expected = load_value(e->loc, bt, ep);
            w = ab->kind == AK_CAS ? load_value(e->loc, bt, expr(args->items[2])) : atomic_to_bits(expr(args->items[2]), obj);
            for (i = 3; i < args->len; i++) {
                expr(args->items[i]);
            }
            old = atomic_call("cas", n, p, expected, w);
            ok = ir_icmp(F, IR_EQ, ty(bt), old, expected);
            store_value(bt, ep, old);
            return ok;
        }
        case AK_FETCH: {
            Type *t = obj->kind == TY_POINTER ? type_basic(TY_ULONG) : obj;
            v = convert(expr(args->items[1]), args->items[1]->type, t);
            if (ab->scaled && obj->kind == TY_POINTER && type_size(obj->base) > 1) {
                v = ir_binop_imm(F, IR_MUL, IR_I64, v, (long)type_size(obj->base));
            }
            for (i = 2; i < args->len; i++) {
                expr(args->items[i]);
            }
            old = atomic_from_bits(atomic_call(ab->op, n, p, atomic_to_bits(v, t), -1), t);
            if (ab->new_value) {
                old = atomic_apply(ab->op, t, old, v);
            }
            return obj->kind == TY_POINTER ? convert(old, t, obj) : old;
        }
        default:
            return -1;
    }
}

static int is_overflow_builtin(const char *name) {
    return strcmp(name, "__builtin_add_overflow") == 0 || strcmp(name, "__builtin_sub_overflow") == 0
        || strcmp(name, "__builtin_mul_overflow") == 0 || strcmp(name, "__builtin_add_overflow_p") == 0
        || strcmp(name, "__builtin_sub_overflow_p") == 0 || strcmp(name, "__builtin_mul_overflow_p") == 0;
}

/*
 * __builtin_{add,sub,mul}_overflow(a, b, &r): the exact result is computed
 * in 128 bits and stored in r's type; the flag says whether the stored
 * value, widened again, is still the exact result. Two unsigned 64 bit
 * factors can exceed a signed 128 bit product, so they multiply unsigned;
 * every other case is exact in a signed one. The _p forms take, in place
 * of &r, an expression whose type is the result's; it is not evaluated,
 * and nothing is stored.
 */
static int overflow_builtin(AST *e, const char *name) {
    ASTList *args = e->u.call.args;
    AST *a;
    AST *b;
    AST *p;
    Type *ta;
    Type *tb;
    Type *tr;
    Type *w;
    IROp op = name[10] == 'a' ? IR_ADD : name[10] == 's' ? IR_SUB : IR_MUL;
    int wa, wb, exact, narrow, back, pv;

    int pred = name[strlen(name) - 1] == 'p';

    if (args->len != 3 || !type_is_integer(args->items[0]->type) || !type_is_integer(args->items[1]->type)
            || (pred ? !type_is_integer(args->items[2]->type)
                : !type_is_pointer(args->items[2]->type) || !type_is_integer(args->items[2]->type->base))) {
        diag_error(e->loc, "'%s' takes two integers and %s", name, pred ? "a third" : "a pointer to an integer");
        return iconst(IR_I32, 0);
    }
    a = args->items[0];
    b = args->items[1];
    p = args->items[2];
    ta = a->type;
    tb = b->type;
    tr = type_unqualified(pred ? p->type : p->type->base);
    w = type_basic(op == IR_MUL && type_is_unsigned(ta) && type_size(ta) >= 8 && type_is_unsigned(tb) && type_size(tb) >= 8
                   ? TY_UINT128 : TY_INT128);
    wa = convert(expr(a), ta, w);
    wb = convert(expr(b), tb, w);
    exact = ir_binop(F, op, IR_I128, wa, wb);
    narrow = convert(exact, w, tr);
    if (!pred) {
        pv = expr(p);
        store_value(tr, pv, narrow);
    }
    back = convert(narrow, tr, w);
    return ir_icmp(F, IR_NE, IR_I128, back, exact);
}

/* ------------------------------------------------------------------ */
/* floating point classification: __builtin_isnan and the rest          */
/* ------------------------------------------------------------------ */

/* (c ? x : y) for i32 values, c being 0 or 1 */
static int select_i32(int c, int x, int y) {
    int m = ir_unop(F, IR_NEG, IR_I32, c);
    return ir_binop(F, IR_XOR, IR_I32, y, ir_binop(F, IR_AND, IR_I32, ir_binop(F, IR_XOR, IR_I32, x, y), m));
}

static int fcmp(OpKind op, Type *t, int a, int b) {
    return arith(op, t, a, b);
}

static int is_inf(Type *t, int v) {
    return ir_binop(F, IR_OR, IR_I32, fcmp(OP_EQ, t, v, float_const(HUGE_VAL, t)), fcmp(OP_EQ, t, v, float_const(-HUGE_VAL, t)));
}

/*
 * The sign bit, and whether the exponent is all zeros or all ones, from the
 * value's bytes: a float or double is one word, the x87 long double keeps
 * its sign and exponent in the 16 bits after the 8 of its mantissa.
 */
static void fp_fields(Type *t, int v, int *sign, int *exp_zero, int *exp_ones) {
    long slot = alloc_slot(16, 16);
    size_t n = type_size(t);
    int w, e;
    long mask;
    store_value(t, addr_local(slot), v);
    if (ty(t) == IR_F80) {
        w = ir_load(F, IR_I32, addr_local(slot + 8), 2, 0);
        *sign = ir_binop_imm(F, IR_LSHR, IR_I32, w, 15);
        e = ir_binop_imm(F, IR_AND, IR_I32, w, 0x7fff);
        mask = 0x7fff;
    } else if (n == 4) {
        w = ir_load(F, IR_I32, addr_local(slot), 4, 0);
        *sign = ir_binop_imm(F, IR_LSHR, IR_I32, w, 31);
        e = ir_binop_imm(F, IR_AND, IR_I32, ir_binop_imm(F, IR_LSHR, IR_I32, w, 23), 0xff);
        mask = 0xff;
    } else {
        w = ir_load(F, IR_I64, addr_local(slot), 8, 0);
        *sign = trunc_to(IR_I32, ir_binop_imm(F, IR_LSHR, IR_I64, w, 63));
        e = trunc_to(IR_I32, ir_binop_imm(F, IR_AND, IR_I64, ir_binop_imm(F, IR_LSHR, IR_I64, w, 52), 0x7ff));
        mask = 0x7ff;
    }
    *exp_zero = ir_icmp_imm(F, IR_EQ, IR_I32, e, 0);
    *exp_ones = ir_icmp_imm(F, IR_EQ, IR_I32, e, mask);
}

/* the GNU spellings of the C99 classification macros (and isinff, isnanl, ...); returns 1 when `b` (past __builtin_) is one */
static int fp_builtin(AST *e, const char *b, int *result) {
    static const char *const names[] = {
        "isnan", "isinf", "isinf_sign", "isfinite", "finite", "isnormal", "signbit", "fpclassify",
        "isgreater", "isgreaterequal", "isless", "islessequal", "islessgreater", "isunordered", NULL
    };
    ASTList *args = e->u.call.args;
    char name[32];
    size_t i, n = strlen(b);
    size_t nargs;
    AST *x;
    Type *t;
    int v;

    if (n >= sizeof(name)) {
        return 0;
    }
    strcpy(name, b);
    for (i = 0; names[i] && strcmp(names[i], name) != 0; i++) {
    }
    if (!names[i] && n > 1 && (name[n - 1] == 'f' || name[n - 1] == 'l')) {
        name[n - 1] = '\0';     /* isinff, isnanl, signbitf, finitel: the same, by the argument's type */
        for (i = 0; names[i] && strcmp(names[i], name) != 0; i++) {
        }
        if (names[i] && (strcmp(name, "isnan") != 0 && strcmp(name, "isinf") != 0 && strcmp(name, "signbit") != 0
                && strcmp(name, "finite") != 0)) {
            return 0;
        }
    }
    if (!names[i]) {
        return 0;
    }
    nargs = strcmp(name, "fpclassify") == 0 ? 6 : strncmp(name, "isgreater", 9) == 0 || strncmp(name, "isless", 6) == 0
            || strcmp(name, "isunordered") == 0 ? 2 : 1;
    if (args->len != nargs || !type_is_floating(args->items[nargs - 1]->type) || type_is_complex(args->items[nargs - 1]->type)) {
        diag_error(e->loc, "'__builtin_%s' takes %s", b, nargs == 6 ? "five integers and a real floating value"
                   : nargs == 2 ? "two real floating values" : "a real floating value");
        *result = iconst(IR_I32, 0);
        return 1;
    }
    if (nargs == 2) {
        AST *y = args->items[1];
        int a, c;
        x = args->items[0];
        if (!type_is_floating(x->type) && !type_is_integer(x->type)) {
            diag_error(e->loc, "'__builtin_%s' takes two real floating values", b);
            *result = iconst(IR_I32, 0);
            return 1;
        }
        t = type_arith_common(x->type, y->type);
        a = convert(expr(x), x->type, t);
        c = convert(expr(y), y->type, t);
        if (strcmp(name, "isunordered") == 0) {
            *result = ir_binop(F, IR_OR, IR_I32, fcmp(OP_NE, t, a, a), fcmp(OP_NE, t, c, c));
        } else if (strcmp(name, "islessgreater") == 0) {
            *result = ir_binop(F, IR_OR, IR_I32, fcmp(OP_LT, t, a, c), fcmp(OP_GT, t, a, c));
        } else {
            OpKind op = strcmp(name, "isgreater") == 0 ? OP_GT : strcmp(name, "isgreaterequal") == 0 ? OP_GE
                        : strcmp(name, "isless") == 0 ? OP_LT : OP_LE;
            *result = fcmp(op, t, a, c);
        }
        return 1;
    }
    x = args->items[nargs - 1];
    t = x->type;
    if (nargs == 6) {
        int k[5];
        int nan, inf, zero, sign, exp_zero, exp_ones;
        for (i = 0; i < 5; i++) {
            k[i] = convert(expr(args->items[i]), args->items[i]->type, type_basic(TY_INT));
        }
        v = expr(x);
        nan = fcmp(OP_NE, t, v, v);
        inf = is_inf(t, v);
        zero = fcmp(OP_EQ, t, v, float_const(0, t));
        fp_fields(t, v, &sign, &exp_zero, &exp_ones);
        /* FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, in the order of the arguments */
        *result = select_i32(nan, k[0], select_i32(inf, k[1], select_i32(zero, k[4], select_i32(exp_zero, k[3], k[2]))));
        return 1;
    }
    v = expr(x);
    if (strcmp(name, "isnan") == 0) {
        *result = fcmp(OP_NE, t, v, v);
    } else if (strcmp(name, "isinf") == 0) {
        *result = is_inf(t, v);
    } else if (strcmp(name, "isinf_sign") == 0) {
        *result = ir_binop(F, IR_SUB, IR_I32, fcmp(OP_EQ, t, v, float_const(HUGE_VAL, t)), fcmp(OP_EQ, t, v, float_const(-HUGE_VAL, t)));
    } else if (strcmp(name, "isfinite") == 0 || strcmp(name, "finite") == 0) {
        /* x - x is 0 for every finite x, NaN for an infinity or a NaN */
        int d = ir_binop(F, IR_FSUB, ty(t), v, v);
        *result = fcmp(OP_EQ, t, d, float_const(0, t));
    } else {
        int sign, exp_zero, exp_ones;
        fp_fields(t, v, &sign, &exp_zero, &exp_ones);
        if (strcmp(name, "signbit") == 0) {
            *result = sign;
        } else {
            /* isnormal: neither zero nor subnormal (exponent all zeros), nor infinite nor NaN (all ones) */
            *result = ir_icmp_imm(F, IR_EQ, IR_I32, ir_binop(F, IR_OR, IR_I32, exp_zero, exp_ones), 0);
        }
    }
    return 1;
}

/*
 * The __builtin_* functions the system headers expand to. The checked
 * string functions exist in libc under their own names; the rest are
 * folded here. Returns 1 when the call has been fully handled, with its
 * value in *result.
 */
static int builtin(AST *e, const char *name, const char **target, int *result) {
    ASTList *args = e->u.call.args;
    const char *b = name + 10;  /* past __builtin_ */

    *result = -1;
    if (is_overflow_builtin(name)) {
        *result = overflow_builtin(e, name);
        return 1;
    }
    if (strcmp(name, "__sync_synchronize") == 0) {
        ir_emit(F, IR_FENCE, IR_VOID, -1, -1, -1);
        return 1;
    }
    if (strcmp(name, "__builtin_shuffle") == 0 && (args->len == 2 || args->len == 3)) {
        Type *vt = args->items[0]->type;
        Type *mt = args->items[args->len - 1]->type;
        Type *et = vt->base;
        long n = vt->length;
        long total = args->len == 3 ? 2 * n : n;
        size_t es = type_size(et);
        int src = vec_temp(type_vector(et, (size_t)total * es) ? type_vector(et, (size_t)total * es) : vt);
        int res = vec_temp(e->type);
        int m;
        long k;
        /* the operands side by side, then each picked element */
        copy_bytes(src, expr(args->items[0]), type_size(vt));
        if (args->len == 3) {
            copy_bytes(ir_binop_imm(F, IR_ADD, IR_I64, src, (long)type_size(vt)), expr(args->items[1]), type_size(vt));
        }
        m = expr(args->items[args->len - 1]);
        for (k = 0; k < mt->length; k++) {
            int idx = convert(vec_elem(m, mt, k), mt->base, type_basic(TY_ULONG));
            int at;
            idx = ir_binop_imm(F, IR_UREM, IR_I64, idx, total);
            at = ir_binop(F, IR_ADD, IR_I64, src, ir_binop_imm(F, IR_MUL, IR_I64, idx, (long)es));
            store_value(et, ir_binop_imm(F, IR_ADD, IR_I64, res, k * (long)es), load_value(e->loc, et, at));
        }
        *result = res;
        return 1;
    }
    if (strcmp(name, "__builtin_apply_args") == 0) {
        if (!F->apply_args_off) {
            diag_error(e->loc, "__builtin_apply_args is not supported on this target");
            *result = iconst(IR_I64, 0);
            return 1;
        }
        *result = addr_local(F->apply_args_off);
        return 1;
    }
    if (strcmp(name, "__builtin_apply") == 0 && args->len == 3) {
        /* the helper calls fn with the saved registers and `size` bytes of the stack arguments; the results' block */
        int vals[4];
        Type *types[4];
        int res = addr_local(alloc_slot(48, 16));
        if (strcmp(arch_info->name, "x86_64") != 0) {
            diag_error(e->loc, "__builtin_apply is not supported on this target");
            *result = iconst(IR_I64, 0);
            return 1;
        }
        vals[0] = convert(expr(args->items[0]), args->items[0]->type, type_pointer(type_basic(TY_VOID)));
        vals[1] = expr(args->items[1]);
        vals[2] = convert(expr(args->items[2]), args->items[2]->type, type_basic(TY_ULONG));
        vals[3] = res;
        types[0] = types[1] = types[3] = type_pointer(type_basic(TY_VOID));
        types[2] = type_basic(TY_ULONG);
        codegen_use_atomic("apply", 0);
        emit_call(ir_name(F, "__jcc_atomic_apply_0"), -1, 4, vals, types, NULL, 4, 0, type_basic(TY_VOID), -1, -1);
        *result = res;
        return 1;
    }
    if (strcmp(name, "__builtin_return") == 0 && args->len == 1) {
        IRInst *i;
        if (strcmp(arch_info->name, "x86_64") != 0) {
            diag_error(e->loc, "__builtin_return is not supported on this target");
            return 1;
        }
        i = ir_inst(F, ir_emit(F, IR_RET, IR_I64, -1, expr(args->items[0]), -1));
        i->flags |= IRF_APPLY;
        return 1;
    }
    if (strcmp(name, "__builtin_va_arg_pack_len") == 0) {
        if (!inline_ctx) {
            diag_error(e->loc, "__builtin_va_arg_pack_len used outside a function inlined at its call");
        }
        *result = iconst(IR_I32, inline_ctx ? inline_ctx->npack : 0);
        return 1;
    }
    if (strcmp(name, "__builtin_setjmp") == 0 && args->len == 1) {
        /*
         * buf[0] the frame pointer, buf[1] where __builtin_longjmp resumes (this
         * call's value is 1 there, 0 now), buf[2] the stack pointer. The function
         * keeps its locals in memory and saves every callee-saved register.
         */
        int b = expr(args->items[0]);
        int recv = ir_block(F);
        int join = ir_block(F);
        int id = codegen_new_label_id();
        int r = ir_vreg(F, IR_I32);
        int la = ir_vreg(F, IR_I64);
        int sp = ir_vreg(F, IR_I64);
        IRInst *i;
        F->blocks[recv].addr_label = id;
        F->save_all = 1;
        nested_call_seen = 1;
        ir_store(F, IR_I64, b, addr_local(0), 8);
        i = ir_inst(F, ir_emit(F, IR_LABEL_ADDR, IR_I64, la, -1, -1));
        i->imm = id;
        ir_store(F, IR_I64, ir_binop_imm(F, IR_ADD, IR_I64, b, 8), la, 8);
        ir_emit(F, IR_SPSAVE, IR_I64, sp, -1, -1);
        ir_store(F, IR_I64, ir_binop_imm(F, IR_ADD, IR_I64, b, 16), sp, 8);
        ir_mov(F, r, iconst(IR_I32, 0));
        ir_br(F, join);
        ir_place(F, recv);
        ir_mov(F, r, iconst(IR_I32, 1));
        ir_place(F, join);
        *result = r;
        return 1;
    }
    if (strcmp(name, "__builtin_longjmp") == 0 && args->len == 2) {
        int b = expr(args->items[0]);
        IRInst *i;
        expr(args->items[1]);   /* 1, as GCC requires */
        i = ir_inst(F, ir_emit(F, IR_NLGOTO, IR_VOID, -1, b, -1));
        i->imm = -1;
        nested_call_seen = 1;
        return 1;
    }
    {
        const AtomicBuiltin *ab = sema_atomic_builtin(name);
        if (ab) {
            *result = atomic_builtin(e, ab);
            return 1;
        }
    }
    if (strncmp(name, "__builtin_", 10) != 0) {
        return 0;
    }
    if (strcmp(b, "va_start") == 0) {
        if (args->len >= 1) {
            ir_emit(F, IR_VASTART, IR_VOID, -1, va_addr(args->items[0]), -1);
        }
        return 1;
    }
    if (strcmp(b, "va_end") == 0) {
        if (args->len >= 1) {
            va_addr(args->items[0]);    /* nothing to do, but the argument's side effects happen */
        }
        return 1;
    }
    if (strcmp(b, "va_copy") == 0) {
        if (args->len == 2) {
            int d = va_addr(args->items[0]);
            int s = va_addr(args->items[1]);
            copy_bytes(d, s, arch_info->va_list_size);
        }
        return 1;
    }
    if (strncmp(b, "bswap", 5) == 0) {
        if (args->len == 1) {
            int v = expr(args->items[0]);
            IRInst *i;
            *result = ir_vreg(F, ty(e->type));
            i = ir_inst(F, ir_emit(F, IR_BSWAP, ty(e->type), *result, v, -1));
            i->size = (unsigned char)type_size(e->type);
            *result = canon(*result, e->type);
        }
        return 1;
    }
    if (strncmp(b, "clz", 3) == 0 || strncmp(b, "ctz", 3) == 0) {
        if (args->len == 1) {
            Type *at = args->items[0]->type;
            int v = expr(args->items[0]);
            int r = ir_unop(F, b[1] == 'l' ? IR_CLZ : IR_CTZ, ty(at), v);
            *result = convert(r, at, e->type);
        }
        return 1;
    }
    if (strcmp(b, "object_size") == 0) {
        long kind = 0;
        if (args->len == 2) {
            sema_const_eval(args->items[1], &kind);
        }
        *result = iconst(ty(e->type), (kind & 2) ? 0 : -1);
        return 1;
    }
    if (strcmp(b, "expect") == 0 || strcmp(b, "expect_with_probability") == 0) {
        /* a hint: the value is the first operand's, though the others are still evaluated */
        size_t k;
        if (args->len >= 1) {
            *result = expr(args->items[0]);
        }
        for (k = 1; k < args->len; k++) {
            expr(args->items[k]);
        }
        return 1;
    }
    if (strcmp(b, "unreachable") == 0) {
        ir_emit(F, IR_UNREACHABLE, IR_VOID, -1, -1, -1);
        return 1;
    }
    if (strcmp(b, "alloca") == 0) {
        if (args->len == 1) {
            int n = convert(expr(args->items[0]), args->items[0]->type, type_basic(TY_ULONG));
            *result = ir_unop(F, IR_ALLOCA, IR_I64, n);
        }
        return 1;
    }
    if (strcmp(b, "abort") == 0 || strcmp(b, "trap") == 0) {
        *target = "abort";
        return 0;
    }
    if (strncmp(b, "creal", 5) == 0 || strncmp(b, "cimag", 5) == 0) {
        if (args->len == 1 && type_is_complex(args->items[0]->type)) {
            Type *base = type_complex_base(args->items[0]->type);
            int a = expr(args->items[0]);
            if (b[1] == 'i') {
                a = ir_binop_imm(F, IR_ADD, IR_I64, a, (long)type_size(base));
            }
            *result = convert(load_value(e->loc, base, a), base, e->type);
        }
        return 1;
    }
    if (strncmp(b, "conj", 4) == 0) {
        if (args->len == 1 && type_is_complex(args->items[0]->type)) {
            /* a copy in the call's own slot, its imaginary part negated in place */
            Type *base = type_complex_base(e->type);
            long cs = (long)type_size(base);
            int slot = addr_local(e->temp);
            int im = addr_local(e->temp + cs);
            int v;
            copy_bytes(slot, expr(args->items[0]), 2 * (size_t)cs);
            v = load_value(e->loc, base, im);
            store_value(base, im, ir_unop(F, IR_FNEG, ty(base), v));
            *result = slot;
        }
        return 1;
    }
    if (b[0] == '_' && b[1] == '_') {
        *target = b;    /* __builtin___memcpy_chk is __memcpy_chk in libc */
        return 0;
    }
    if (strncmp(b, "inf", 3) == 0 || strncmp(b, "huge_val", 8) == 0 || strncmp(b, "nan", 3) == 0) {
        /* infinity or a quiet NaN of the result type, as a constant */
        long double inf = HUGE_VAL;
        *result = float_const(b[0] == 'n' ? inf - inf : inf, e->type);
        return 1;
    }
    if (fp_builtin(e, b, result)) {
        return 1;
    }
    if (strcmp(b, "prefetch") == 0) {
        if (args->len >= 1) {
            expr(args->items[0]);   /* a hint only; the address expression still happens */
        }
        return 1;
    }
    {
        /* the libc function of the same name (sema gave them their types) */
        static const char *const libc[] = {
            "memcpy", "memmove", "memset", "memcmp", "memchr", "strlen", "strcmp", "strncmp",
            "strcpy", "strncpy", "strcat", "strncat", "strchr", "strrchr", "strstr",
            "printf", "sprintf", "snprintf", "puts", "putchar",
            "malloc", "calloc", "realloc", "free", "exit",
            "abs", "labs", "llabs", "ffs", "ffsl", "ffsll",
            "fabs", "fabsf", "fabsl", "copysign", "copysignf", "copysignl",
            "sqrt", "sqrtf", "floor", "floorf", "ceil", "ceilf", "fmod", "fmodf",
            "bzero", "bcopy", NULL
        };
        size_t i;
        for (i = 0; libc[i]; i++) {
            if (strcmp(b, libc[i]) == 0) {
                *target = b;
                return 0;
            }
        }
    }
    unsupported(e->loc, name);
    return 1;
}

/* does a call to a function returning `t` pass a hidden pointer for the result */
static int returns_via_sret(Type *t) {
    ArgType at;
    if (!by_address(t)) {
        return 0;
    }
    codegen_arg_type(t, &at);
    return !cg->record_ret_regs(&at);
}

/*
 * At -O1, sqrt, fabs and (where the target has them) floor, ceil, trunc
 * and round of a double or float are instructions, as clang makes them:
 * the libc of either target sets no errno for them. Returns the result,
 * or -1 when the call is not one of these.
 */
static int math_intrinsic(AST *e, const char *name) {
    static const struct { const char *name; IROp op; int mode; } fns[] = {
        { "sqrt", IR_FSQRT, 0 }, { "fabs", IR_FABS, 0 },
        { "floor", IR_FRINT, 0 }, { "ceil", IR_FRINT, 1 }, { "trunc", IR_FRINT, 2 }, { "round", IR_FRINT, 3 }
    };
    AST *a = e->u.call.args->items[0];
    IRType t = ty(e->type);
    size_t i, n;
    for (i = 0; i < sizeof(fns) / sizeof(fns[0]); i++) {
        n = strlen(fns[i].name);
        if (strncmp(name, fns[i].name, n) != 0 || (name[n] != '\0' && !(name[n] == 'f' && name[n + 1] == '\0'))) {
            continue;
        }
        if (fns[i].op == IR_FRINT && !arch_info->fp_round) {
            return -1;
        }
        if ((t != IR_F64 && t != IR_F32) || ty(a->type) != t || (t == IR_F32) != (name[n] == 'f')) {
            return -1;
        }
        {
            int v = expr(a);
            int dst = ir_vreg(F, t);
            IRInst *ins = ir_inst(F, ir_emit(F, fns[i].op, t, dst, v, -1));
            ins->imm = fns[i].mode;
            return dst;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* GNU __builtin_va_arg_pack: functions inlined at their calls          */
/* ------------------------------------------------------------------ */

/*
 * A function that passes its extra arguments on with __builtin_va_arg_pack
 * (always_inline, as GCC requires) is lowered as part of each caller: its
 * parameters are the caller's locals, a return stores the result and goes
 * to the end, and the pack is the call's own extra arguments, evaluated
 * at the call.
 */


static void mark_memory(AST *n);

static int is_va_arg_pack(const AST *a) {
    Symbol *c;
    if (a->kind != N_CALL) {
        return 0;
    }
    c = direct_callee(a->u.call.func);
    return c && strcmp(c->name, "__builtin_va_arg_pack") == 0;
}

static int calls_pack(const AST *n);

/* does the tree call a function whose name begins with `prefix`? */
static int calls_named(const AST *n, const char *prefix) {
    const char *saved = calls_prefix;
    int r;
    calls_prefix = prefix;
    r = calls_pack(n);
    calls_prefix = saved;
    return r;
}

/* does the tree call __builtin_va_arg_pack (or __builtin_va_arg_pack_len), or what calls_prefix names? */
static int calls_pack(const AST *n) {
    size_t i;
    if (!n) {
        return 0;
    }
    switch (n->kind) {
        case N_CALL: {
            Symbol *c = direct_callee(n->u.call.func);
            if (c && strncmp(c->name, calls_prefix, strlen(calls_prefix)) == 0) {
                return 1;
            }
            for (i = 0; i < n->u.call.args->len; i++) {
                if (calls_pack(n->u.call.args->items[i])) {
                    return 1;
                }
            }
            return calls_pack(n->u.call.func);
        }
        case N_UNARY:
            return calls_pack(n->u.unary.operand);
        case N_BINARY:
        case N_ASSIGN:
            return calls_pack(n->u.binary.left) || calls_pack(n->u.binary.right);
        case N_COND:
        case N_IF:
            return calls_pack(n->u.cond.cond) || calls_pack(n->u.cond.then) || calls_pack(n->u.cond.otherwise);
        case N_INDEX:
            return calls_pack(n->u.index.array) || calls_pack(n->u.index.index);
        case N_MEMBER:
            return calls_pack(n->u.member.object);
        case N_CAST:
        case N_IMPLICIT_CAST:
            return calls_pack(n->u.cast.expr);
        case N_COMPOUND:
            for (i = 0; i < n->u.block.items->len; i++) {
                if (calls_pack(n->u.block.items->items[i])) {
                    return 1;
                }
            }
            return 0;
        case N_DECLARATION:
            for (i = 0; i < n->u.decl.declarators->len; i++) {
                if (calls_pack(n->u.decl.declarators->items[i]->u.init_decl.init)) {
                    return 1;
                }
            }
            return 0;
        case N_STMT_EXPR:
            return calls_pack(n->u.stmt.body);
        case N_LABEL: case N_CASE: case N_DEFAULT: case N_EXPR_STMT: case N_SWITCH: case N_WHILE: case N_DO: case N_RETURN:
            return calls_pack(n->u.stmt.expr) || calls_pack(n->u.stmt.body);
        case N_FOR:
            return calls_pack(n->u.loop.init) || calls_pack(n->u.loop.cond) || calls_pack(n->u.loop.step)
                || calls_pack(n->u.loop.body);
        default:
            return 0;
    }
}

int lower_uses_va_arg_pack(AST *fdef) {
    return fdef && fdef->kind == N_FUNCTION_DEF && calls_pack(fdef->u.func.body);
}

static void return_stmt(AST *s);

static int inline_call(AST *e, Symbol *callee) {
    AST *fdef = callee->def;
    AST *fn = ast_declarator_innermost(fdef->u.func.declarator);
    ASTList *params = fn ? fn->u.declarator.params : NULL;
    ASTList *args = e->u.call.args;
    size_t nparams = params && fn->u.declarator.kr == 0 ? params->len : 0;
    InlineCtx ctx;
    int *vals;
    size_t i;
    int saved_break = break_block, saved_continue = continue_block;
    SwitchGen *saved_switch = cur_switch;
    Symbol *s;

    if (nparams == 1 && params->items[0]->u.typed.declarator == NULL && type_is_void(params->items[0]->type)) {
        nparams = 0;    /* (void) */
    }
    for (s = fdef->u.func.scope ? fdef->u.func.scope->first : NULL; s; s = s->next) {
        if (s->ns == NS_LABEL) {
            s->label = 0;   /* its blocks are new in each caller */
        }
    }
    /* the arguments, each once, in order; a record's copied, as a call's would be */
    vals = xrealloc(NULL, (args->len + 1) * sizeof(int));
    for (i = 0; i < args->len; i++) {
        AST *a = args->items[i];
        vals[i] = expr(a);
        if (by_address(a->type)) {
            int copy = addr_local(alloc_slot(type_size(a->type), type_align(a->type) > 16 ? 16 : type_align(a->type)));
            copy_object(copy, vals[i], a->type);
            vals[i] = copy;
        }
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.outer = inline_ctx;
    ctx.ret = e->type;
    ctx.result = -1;
    ctx.npack = args->len > nparams ? (int)(args->len - nparams) : 0;
    ctx.pack_vals = vals + nparams;
    ctx.pack_by_addr = xrealloc(NULL, ((size_t)ctx.npack + 1) * sizeof(int));
    ctx.pack_types = xrealloc(NULL, ((size_t)ctx.npack + 1) * sizeof(Type *));
    for (i = nparams; i < args->len; i++) {
        ctx.pack_by_addr[i - nparams] = by_address(args->items[i]->type);
        ctx.pack_types[i - nparams] = args->items[i]->type;
    }
    if (!type_is_void(e->type)) {
        ctx.result = by_address(e->type) ? addr_local(alloc_slot(type_size(e->type), 16)) : ir_vreg(F, ty(e->type));
    }
    /* the parameters: locals of this function now (in memory, simply) */
    mark_memory(fdef->u.func.body);
    for (i = 0; i < nparams && i < args->len; i++) {
        AST *pd = params->items[i];
        Symbol *p = pd->sym;
        if (!p || !p->type) {
            continue;
        }
        p->vreg = 0;
        p->by_ref = 0;
        alloc_local(p);
        if (by_address(p->type)) {
            copy_object(addr_local(p->offset), vals[i], p->type);
        } else {
            store_value(p->type, addr_local(p->offset), convert(vals[i], args->items[i]->type, p->type));
        }
    }
    assign_locals(fdef->u.func.body);
    ctx.end = ir_block(F);
    inline_ctx = &ctx;
    break_block = continue_block = -1;
    stmt(fdef->u.func.body);
    inline_ctx = ctx.outer;
    break_block = saved_break;
    continue_block = saved_continue;
    cur_switch = saved_switch;
    ir_place(F, ctx.end);
    free(vals);
    free(ctx.pack_by_addr);
    free(ctx.pack_types);
    return ctx.result;
}

static int call(AST *e) {
    AST *f = e->u.call.func;
    Type *fn = f->type->base;
    ASTList *args = e->u.call.args;
    Symbol *callee = direct_callee(f);
    const char *name = callee ? callee->name : NULL;
    size_t nfixed;
    size_t i;
    int nvals = 0;
    int *vals = NULL;
    int *by_addr = NULL;
    Type **types = NULL;
    int target = -1;
    int sret_addr = -1;
    int result_addr = -1;
    int result;
    int sret = returns_via_sret(e->type);

    if (!check_arg_type(e->loc, e->type)) {
        return iconst(IR_I32, 0);
    }
    for (i = 0; i < args->len; i++) {
        if (!check_arg_type(args->items[i]->loc, args->items[i]->type)) {
            return iconst(IR_I32, 0);
        }
    }
    if (name && strcmp(name, "alloca") == 0 && !callee->def) {
        name = "__builtin_alloca";   /* declared but not defined here: the builtin, as GCC has it */
    }
    if (name && (strncmp(name, "__builtin_", 10) == 0 || strncmp(name, "__atomic_", 9) == 0
            || strncmp(name, "__sync_", 7) == 0 || strncmp(name, "__c11_atomic_", 13) == 0) && builtin(e, name, &name, &result)) {
        return result;
    }

    if (name && codegen_opt_level > 0 && (!callee || !callee->def) && args->len == 1 && (result = math_intrinsic(e, name)) >= 0) {
        return result;
    }

    if (callee && callee->def && lower_uses_va_arg_pack(callee->def)) {
        return inline_call(e, callee);      /* __builtin_va_arg_pack: only as part of the caller */
    }
    nfixed = fn->has_proto && fn->variadic ? fn->nparams : args->len;
    {
        size_t cap = args->len + (inline_ctx ? (size_t)inline_ctx->npack : 0) + 1;
        vals = xrealloc(NULL, cap * sizeof(*vals));
        by_addr = xrealloc(NULL, cap * sizeof(*by_addr));
        types = xrealloc(NULL, cap * sizeof(*types));
    }
    for (i = 0, nvals = 0; i < args->len; i++, nvals++) {
        AST *a = args->items[i];
        if (is_va_arg_pack(a)) {
            /* the inlined function's own extra arguments, here */
            int k;
            if (!inline_ctx) {
                diag_error(a->loc, "__builtin_va_arg_pack used outside a function inlined at its call");
                nvals--;
                continue;
            }
            for (k = 0; k < inline_ctx->npack; k++) {
                vals[nvals] = inline_ctx->pack_vals[k];
                by_addr[nvals] = inline_ctx->pack_by_addr[k];
                types[nvals] = inline_ctx->pack_types[k];
                nvals++;
            }
            nvals--;
            continue;
        }
        vals[nvals] = expr(a);
        by_addr[nvals] = by_address(a->type);
        types[nvals] = a->type;
        if (type_is_record(a->type) && a->type->record->vm) {
            /* of a size known at run time: a copy's address, as GCC passes one */
            int copy = ir_unop(F, IR_ALLOCA, IR_I64, size_of(a->type));
            copy_object(copy, vals[nvals], a->type);
            vals[nvals] = copy;
            by_addr[nvals] = 0;
            types[nvals] = type_pointer(type_basic(TY_VOID));
        }
    }
    if (!callee) {
        target = expr(f);
    }
    if (by_address(e->type)) {
        /* the result lives in this call's temporary; a record evaluates to its address */
        if (sret) {
            sret_addr = temp_addr(e);
        } else {
            result_addr = temp_addr(e);
        }
    }
    if (callee && name == callee->name) {
        name = ir_name(F, codegen_sym_name(callee));    /* its assembler name: an asm label's, or an alias's target */
    }
    if (callee && callee->parent_fn && name) {
        /* a nested function: its static chain, through a slot the optimizer is kept away from */
        long slot = alloc_slot(8, 8);
        store_value(type_pointer(type_basic(TY_VOID)), addr_local(slot), frame_of(callee->parent_fn));
        result = emit_call(name, target, nvals, vals, types, by_addr, nfixed, fn->variadic, e->type, sret_addr, result_addr);
        F->calls[F->ncalls - 1].chain_slot = slot;
        F->noopt = 1;
        nested_call_seen = 1;
    } else {
        result = emit_call(name, target, nvals, vals, types, by_addr, nfixed, fn->variadic, e->type, sret_addr, result_addr);
    }
    free(vals);
    free(by_addr);
    free(types);
    if (by_address(e->type)) {
        return sret ? sret_addr : result_addr;
    }
    if (result < 0) {
        return -1;
    }
    /* the callee need not have cleaned the upper bits of a narrow result */
    if (e->type->kind == TY_BOOL) {
        return extend(IR_ZEXT, IR_I32, result, 1);
    }
    return canon(result, e->type);
}

static int expr(AST *e) {
    long v;

    switch (e->kind) {
        case N_OFFSETOF: {
            /* one sema could not fold: a run time index, or a variable length type on the way */
            Type *t = e->u.desig.init->type;
            int off = iconst(IR_I64, 0);
            size_t k;
            for (k = 0; k < e->u.desig.path->len; k++) {
                AST *d = e->u.desig.path->items[k];
                if (d->kind == N_DESIG_MEMBER) {
                    Member *path[RECORD_PATH_MAX];
                    size_t depth, j;
                    Member *m = record_lookup(t->record, d->u.name, path, &depth);
                    for (j = 0; j < depth; j++) {
                        off = ir_binop_imm(F, IR_ADD, IR_I64, off, (long)path[j]->offset);
                    }
                    if (m->rt_slot) {
                        off = ir_binop(F, IR_ADD, IR_I64, off, ir_load(F, IR_I64, addr_local(m->rt_slot), 8, 0));
                    } else {
                        off = ir_binop_imm(F, IR_ADD, IR_I64, off, (long)m->offset);
                    }
                    t = m->type;
                } else {
                    int idx = convert(expr(d->u.cast.expr), d->u.cast.expr->type, type_basic(TY_LONG));
                    off = ir_binop(F, IR_ADD, IR_I64, off, ir_binop(F, IR_MUL, IR_I64, idx, size_of(t->base)));
                    t = t->base;
                }
            }
            return off;
        }

        case N_LABEL_ADDR: {
            int dst = ir_vreg(F, IR_I64);
            IRInst *i;
            if (!e->sym) {
                return iconst(IR_I64, 0);
            }
            i = ir_inst(F, ir_emit(F, IR_LABEL_ADDR, IR_I64, dst, -1, -1));
            i->imm = codegen_label_id(e->sym);
            return dst;
        }

        case N_STMT_EXPR: {
            ASTList *items = e->u.stmt.body->u.block.items;
            size_t i;
            int result = -1;
            for (i = 0; i < items->len; i++) {
                AST *item = items->items[i];
                AST *last = item;
                while (i + 1 == items->len && last->kind == N_LABEL && last->u.stmt.body) {
                    last = last->u.stmt.body;
                }
                if (i + 1 == items->len && last->kind == N_EXPR_STMT && last->u.stmt.expr) {
                    /* the value; labels on the statement are placed first */
                    for (; item != last; item = item->u.stmt.body) {
                        if (item->sym) {
                            ir_place(F, label_block(item->sym));
                        }
                    }
                    result = expr(last->u.stmt.expr);
                } else if (item->kind == N_DECLARATION) {
                    local_decl(item);
                } else {
                    stmt(item);
                }
            }
            return result;
        }

        case N_SIZEOF_TYPE:
            if (!e->u.cast.align && e->u.cast.type->type && type_is_vla(e->u.cast.type->type)) {
                vla_eval(e->u.cast.type->u.typed.declarator);
                return size_of(e->u.cast.type->type);
            }
            /* fall through */
        case N_SIZEOF_EXPR:
            if (e->kind == N_SIZEOF_EXPR && !e->u.cast.align && e->u.cast.expr->type && type_is_vla(e->u.cast.expr->type)) {
                return size_of(e->u.cast.expr->type);
            }
            /* fall through */
        case N_INT:
        case N_CHAR:
            if (!sema_const_eval(e, &v)) {
                diag_error(e->loc, "internal error: constant has no value");
                v = 0;
            }
            return iconst(ty(e->type), v);

        case N_FLOAT: {
            long double f;
            if (type_is_complex(e->type)) {
                return complex_const(e);
            }
            if (!lower_check_value_type(e->loc, e->type)) {
                return iconst(IR_I32, 0);
            }
            if (!sema_const_eval_float(e, &f)) {
                diag_error(e->loc, "internal error: constant has no value");
                f = 0;
            }
            if (is_soft(e->type)) {
                unsigned char b[16];
                int t = soft_temp(e->type);
                long w0 = 0, w1 = 0;
                int k;
                if (type_is_decimal(e->type)) {
                    lower_bid_bytes(e->u.text, 0, e->type->kind, b);
                    if (type_size(e->type) < 16) {
                        for (k = (int)type_size(e->type) - 1; k >= 0; k--) {
                            w0 = (long)((unsigned long)w0 << 8 | b[k]);
                        }
                        ir_store(F, type_size(e->type) == 4 ? IR_I32 : IR_I64, t, iconst(type_size(e->type) == 4 ? IR_I32 : IR_I64, w0), type_size(e->type));
                        return t;
                    }
                } else {
                    lower_f128_bytes(f, b);
                }
                for (k = 7; k >= 0; k--) {
                    w0 = (long)((unsigned long)w0 << 8 | b[k]);
                    w1 = (long)((unsigned long)w1 << 8 | b[8 + k]);
                }
                ir_store(F, IR_I64, t, iconst(IR_I64, w0), 8);
                ir_store(F, IR_I64, ir_binop_imm(F, IR_ADD, IR_I64, t, 8), iconst(IR_I64, w1), 8);
                return t;
            }
            return float_const(f, e->type);
        }

        case N_STRING:
            return addr(e);

        case N_IDENT: {
            Symbol *s = e->sym;
            if (!s) {
                return iconst(IR_I32, 0);
            }
            if (s->kind == SYM_ENUMERATOR) {
                return iconst(ty(e->type), s->value);
            }
            if (local_vreg(s) >= 0) {
                return local_vreg(s);
            }
            if (s->kind == SYM_FUNCTION) {
                return addr(e);
            }
            return load_value(e->loc, e->type, addr(e));
        }

        case N_IMPLICIT_CAST:
        case N_CAST:
            return cast(e);

        case N_UNARY:
            return unary(e);

        case N_BINARY:
            return binary(e);

        case N_ASSIGN:
            return assign(e);

        case N_COND:
            return conditional(e);

        case N_CALL:
            return call(e);

        case N_INDEX:
        case N_MEMBER:
            if (e->kind == N_MEMBER && sema_is_bitfield(e)) {
                return load_bits(e->u.member.member, e->type, addr(e));
            }
            if (reversed_lvalue(e) && is_value_type(e->type)) {
                return load_reversed(type_unqualified(e->type), addr(e));     /* scalar_storage_order */
            }
            return load_value(e->loc, e->type, addr(e));

        case N_VA_ARG:
            return va_arg_expr(e);

        case N_COMPOUND_LIT:
            return load_value(e->loc, e->type, compound_lit_addr(e));

        default:
            diag_error(e->loc, "internal error: %s is not an expression", node_kind_name(e->kind));
            return iconst(IR_I32, 0);
    }
}

/* ------------------------------------------------------------------ */
/* complex values                                                     */
/* ------------------------------------------------------------------ */

/*
 * A _Complex float, double or long double is reached by address, exactly
 * as a struct of its two components would be (calls, returns, va_arg and
 * copies go through the record machinery once codegen_arg_type
 * classifies it so). Every complex valued expression node owns a frame
 * slot (assign_temps): the node's result goes there, and a consumer
 * copies the operand's value into the operand's own slot to work on the
 * components at fixed frame offsets.
 */
/* evaluate the complex expression `e` and copy its value into e's slot; the slot's offset is returned */
static long complex_to_slot(AST *e) {
    int v = expr(e);
    copy_bytes(addr_local(e->temp), v, type_size(e->type));
    return e->temp;
}

static int load_comp(long off, Type *base) {
    return ir_load(F, ty(base), addr_local(off), type_size(base), 0);
}

static void store_comp(long off, Type *base, int v) {
    ir_store(F, ty(base), addr_local(off), v, type_size(base));
}

/* an imaginary constant: real part 0 */
static int complex_const(AST *e) {
    Type *base = type_complex_base(e->type);
    long cs = (long)type_size(base);
    long double re, im;
    if (!sema_const_eval_complex(e, &re, &im)) {
        diag_error(e->loc, "internal error: constant has no value");
        re = im = 0;
    }
    store_comp(e->temp, base, float_const(re, base));
    store_comp(e->temp + cs, base, float_const(im, base));
    return addr_local(e->temp);
}

/* the complex value `e` is not 0: either component is nonzero */
static int complex_truth(AST *e) {
    Type *base = type_complex_base(e->type);
    long s = complex_to_slot(e);
    int re = truth(load_comp(s, base), base);
    int im = truth(load_comp(s + (long)type_size(base), base), base);
    return ir_binop(F, IR_OR, IR_I32, re, im);
}

static void complex_truth_branch(AST *e, int bt, int bf) {
    IRInst *i = ir_inst(F, ir_emit(F, IR_CONDBR, IR_I32, -1, complex_truth(e), -1));
    i->imm = bt;
    i->aux = bf;
}

static int complex_cast(AST *e) {
    AST *x = e->u.cast.expr;
    Type *from = x->type;
    Type *to = e->type;
    long k;

    if (!type_is_complex(to)) {
        /* complex to real: the real part; to _Bool: whether it is nonzero (C99 6.3.1.2) */
        Type *base = type_complex_base(from);
        int a;
        if (to->kind == TY_BOOL) {
            return complex_truth(x);
        }
        a = expr(x);
        return convert(ir_load(F, ty(base), a, type_size(base), 0), base, to);
    }
    if (!type_is_complex(from)) {
        /* real to complex: the value, converted, and an imaginary part of 0 */
        Type *base = type_complex_base(to);
        int v = convert(expr(x), from, base);
        store_comp(e->temp, base, v);
        store_comp(e->temp + (long)type_size(base), base, float_const(0, base));
        return addr_local(e->temp);
    }
    if (type_complex_base(from)->kind == type_complex_base(to)->kind) {
        return expr(x);     /* the same representation: the value where it is */
    }
    {
        Type *fb = type_complex_base(from);
        Type *tb = type_complex_base(to);
        long s = complex_to_slot(x);
        for (k = 0; k < 2; k++) {
            int v = convert(load_comp(s + k * (long)type_size(fb), fb), fb, tb);
            store_comp(e->temp + k * (long)type_size(tb), tb, v);
        }
        return addr_local(e->temp);
    }
}

/* unary minus, or ~ (GNU: the conjugate, which negates the imaginary part only) */
static int complex_unary(AST *e) {
    Type *base = type_complex_base(e->type);
    long cs = (long)type_size(base);
    long s = complex_to_slot(e->u.unary.operand);
    long k;
    for (k = 0; k < 2; k++) {
        int v = load_comp(s + k * cs, base);
        if (k == 1 || e->u.unary.op == OP_NEG) {
            v = ir_unop(F, IR_FNEG, ty(base), v);
        }
        store_comp(e->temp + k * cs, base, v);
    }
    return addr_local(e->temp);
}

/*
 * Multiplication and division call the compiler runtime (__muldc3 and
 * friends, as gcc and clang do, for the infinity and NaN rules of C99
 * Annex G): four scalar arguments, the components of the two operands
 * converted to the result's component type, and a complex result that
 * comes back in registers, stored at `result`.
 */
static void complex_runtime(OpKind op, Type *ct, long l, Type *lbase, long r, Type *rbase, long result) {
    Type *base = type_complex_base(ct);
    size_t cs = type_size(base);
    const char *name = cs == 4 ? (op == OP_MUL ? "__mulsc3" : "__divsc3")
        : cs == 8 ? (op == OP_MUL ? "__muldc3" : "__divdc3")
        : (op == OP_MUL ? "__mulxc3" : "__divxc3");
    int args[4];
    Type *types[4];
    args[0] = convert(load_comp(l, lbase), lbase, base);
    args[1] = convert(load_comp(l + (long)type_size(lbase), lbase), lbase, base);
    args[2] = convert(load_comp(r, rbase), rbase, base);
    args[3] = convert(load_comp(r + (long)type_size(rbase), rbase), rbase, base);
    types[0] = types[1] = types[2] = types[3] = base;
    emit_call(name, -1, 4, args, types, NULL, 4, 0, ct, -1, addr_local(result));
}

/* + - * / == != on two complex values of one type (sema converted them) */
static int complex_binary(AST *e) {
    OpKind op = e->u.binary.op;
    AST *l = e->u.binary.left;
    AST *r = e->u.binary.right;
    Type *base = type_complex_base(l->type);
    long cs = (long)type_size(base);
    long rs;
    long ls;
    long k;

    rs = complex_to_slot(r);
    ls = complex_to_slot(l);
    switch (op) {
        case OP_ADD:
        case OP_SUB:
            for (k = 0; k < 2; k++) {
                int a = load_comp(ls + k * cs, base);
                int b = load_comp(rs + k * cs, base);
                int v = ir_binop(F, op == OP_ADD ? IR_FADD : IR_FSUB, ty(base), a, b);
                store_comp(e->temp + k * cs, base, v);
            }
            return addr_local(e->temp);
        case OP_MUL:
        case OP_DIV:
            complex_runtime(op, e->type, ls, base, rs, base, e->temp);
            return addr_local(e->temp);
        case OP_EQ:
        case OP_NE: {
            int eq[2];
            int both;
            for (k = 0; k < 2; k++) {
                IRInst *i;
                int a, b;
                eq[k] = ir_vreg(F, IR_I32);
                a = load_comp(ls + k * cs, base);
                b = load_comp(rs + k * cs, base);
                i = ir_inst(F, ir_emit(F, IR_FCMP, ty(base), eq[k], a, b));
                i->cc = IR_EQ;
            }
            both = ir_binop(F, IR_AND, IR_I32, eq[0], eq[1]);
            if (op == OP_NE) {
                both = ir_icmp_imm(F, IR_EQ, IR_I32, both, 0);
            }
            return both;
        }
        default:
            unsupported(e->loc, "this operator on complex values");
            return iconst(IR_I32, 0);
    }
}

/*
 * A compound assignment to a complex object: the object's address is
 * kept in a register while the operation happens in the common type in
 * the node's slot, then the result is converted and stored back. The
 * value of the expression is the object's address.
 */
static int complex_compound(AST *e) {
    OpKind op = e->u.binary.op;
    AST *l = e->u.binary.left;
    AST *r = e->u.binary.right;
    Type *lt = l->type;
    Type *common = r->type;         /* the complex type sema converted the right operand to */
    Type *lbase = type_complex_base(lt);
    Type *cbase = type_complex_base(common);
    long lcs = (long)type_size(lbase);
    long ccs = (long)type_size(cbase);
    long result = e->temp;
    long rs;
    long k;
    int ptr;

    if (!type_is_complex(common)) {
        unsupported(e->loc, "this compound assignment on a complex object");
        return iconst(IR_I64, 0);
    }
    rs = complex_to_slot(r);
    ptr = addr(l);
    /* the object's current value, into its own slot */
    copy_bytes(addr_local(l->temp), ptr, type_size(lt));
    switch (op) {
        case OP_ADD_ASSIGN:
        case OP_SUB_ASSIGN:
            for (k = 0; k < 2; k++) {
                int lv = convert(load_comp(l->temp + k * lcs, lbase), lbase, cbase);
                int v = ir_binop(F, op == OP_ADD_ASSIGN ? IR_FADD : IR_FSUB, ty(cbase), lv, load_comp(rs + k * ccs, cbase));
                store_comp(result + k * ccs, cbase, v);
            }
            break;
        case OP_MUL_ASSIGN:
        case OP_DIV_ASSIGN:
            complex_runtime(op == OP_MUL_ASSIGN ? OP_MUL : OP_DIV, common, l->temp, lbase, rs, cbase, result);
            break;
        default:
            unsupported(e->loc, "this compound assignment on a complex object");
            return ptr;
    }
    /* back into the object, converted to its type */
    for (k = 0; k < 2; k++) {
        int v = convert(load_comp(result + k * ccs, cbase), cbase, lbase);
        int a = k ? ir_binop_imm(F, IR_ADD, IR_I64, ptr, k * lcs) : ptr;
        ir_store(F, ty(lbase), a, v, type_size(lbase));
    }
    return ptr;
}

/* ------------------------------------------------------------------ */
/* local initializers (the walker is in codegen.c)                    */
/* ------------------------------------------------------------------ */

static int init_addr(long off) {
    if (init_base < 0) {
        return addr_local(off);
    }
    return off == init_base_off ? init_base : ir_binop_imm(F, IR_ADD, IR_I64, init_base, off - init_base_off);
}

void lower_init_scalar(long off, Type *t, AST *e) {
    int v;
    if (!by_address(t) && !lower_check_value_type(e->loc, t)) {
        return;
    }
    v = expr(e);
    if (by_address(t)) {
        copy_bytes(init_addr(off), v, type_size(t));
    } else if (lower_init_reverse && type_size(t) > 1) {
        store_reversed(type_unqualified(t), init_addr(off), v);
    } else {
        store_value(t, init_addr(off), v);
    }
}

void lower_init_string(long off, AST *s, size_t n) {
    /* one at a time: the order arguments are evaluated in is the host compiler's choice */
    int dst = init_addr(off);
    int src = addr_string(codegen_add_string(s));
    copy_bytes(dst, src, n);
}

void lower_init_bitfield(long off, Member *m, AST *item) {
    int a = init_addr(off);
    int v = expr(item);
    store_bits(m, m->type, a, v);
}

/*
 * GNU structs with variable length members. Where the definition is, each
 * gets a slot for its size and one for each member's offset that is only
 * known at run time; when the definition is reached, its variable length
 * members' sizes are worked out and the layout made: each such member at
 * its alignment after what precedes it, the members after it where the
 * static layout put them (which counted it as empty), moved on by the
 * room it takes, rounded to their alignment.
 */
static void vm_record_slots(ASTList *specs) {
    size_t i, k, j;
    for (i = 0; specs && i < specs->len; i++) {
        AST *spec = specs->items[i];
        Record *r;
        if (spec->kind != N_STRUCT_SPEC || !spec->u.record.has_body || !spec->type || !type_is_record(spec->type)
                || !(r = spec->type->record)->vm) {
            continue;
        }
        r->size_slot = alloc_slot(8, 8);
        for (k = 0; k < spec->u.record.members->len; k++) {
            AST *sdecl = spec->u.record.members->items[k];
            if (sdecl->kind != N_STRUCT_DECLARATION) {
                continue;
            }
            vm_record_slots(sdecl->u.decl.specs);
            for (j = 0; j < sdecl->u.decl.declarators->len; j++) {
                vla_slots(sdecl->u.decl.declarators->items[j]->u.init_decl.declarator);
            }
        }
        for (k = 0; k < r->nmembers; k++) {
            r->members[k].rt_slot = 0;
        }
        {
            int seen = 0;
            for (k = 0; k < r->nmembers; k++) {
                if (seen && r->kind == TY_STRUCT) {
                    r->members[k].rt_slot = alloc_slot(8, 8);
                }
                if (type_is_vla(r->members[k].type)) {
                    if (!seen && r->kind == TY_STRUCT) {
                        r->members[k].rt_slot = alloc_slot(8, 8);
                    }
                    seen = 1;
                }
            }
        }
    }
}

static void vm_record_layout(ASTList *specs) {
    size_t i, k, j;
    Type *u = type_basic(TY_ULONG);
    for (i = 0; specs && i < specs->len; i++) {
        AST *spec = specs->items[i];
        Record *r;
        int size;
        if (spec->kind != N_STRUCT_SPEC || !spec->u.record.has_body || !spec->type || !type_is_record(spec->type)
                || !(r = spec->type->record)->vm) {
            continue;
        }
        for (k = 0; k < spec->u.record.members->len; k++) {
            AST *sdecl = spec->u.record.members->items[k];
            if (sdecl->kind != N_STRUCT_DECLARATION) {
                continue;
            }
            vm_record_layout(sdecl->u.decl.specs);
            for (j = 0; j < sdecl->u.decl.declarators->len; j++) {
                vla_eval(sdecl->u.decl.declarators->items[j]->u.init_decl.declarator);
            }
        }
        if (r->kind == TY_UNION) {
            size = iconst(IR_I64, (long)r->size);
            for (k = 0; k < r->nmembers; k++) {
                Member *m = &r->members[k];
                if (type_is_vla(m->type)) {
                    /* the larger of the two */
                    int ms = size_of(m->type);
                    int bigger = ir_icmp(F, IR_UGT, IR_I64, ms, size);
                    int next = ir_vreg(F, IR_I64);
                    int b1 = ir_block(F), b2 = ir_block(F), join = ir_block(F);
                    ir_condbr(F, bigger, b1, b2);
                    ir_place(F, b1);
                    ir_mov(F, next, ms);
                    ir_br(F, join);
                    ir_place(F, b2);
                    ir_mov(F, next, size);
                    ir_place(F, join);
                    size = next;
                }
            }
        } else {
            /*
             * From the first variable length member on, each member at its alignment after the
             * one before (bit-fields that shared a unit in the static layout share one here),
             * as GCC lays them out; the members before it are where the static layout put them.
             */
            int cur = -1;
            int prev_off = -1;
            const Member *prev = NULL;
            for (k = 0; k < r->nmembers; k++) {
                Member *m = &r->members[k];
                size_t al = r->packed ? 1 : type_align(m->type);
                int off;
                if (!m->rt_slot) {
                    continue;
                }
                if (cur < 0) {
                    cur = iconst(IR_I64, (long)m->offset);
                }
                if (m->bits >= 0 && prev && prev->bits >= 0 && prev->offset == m->offset && prev_off >= 0) {
                    off = prev_off;     /* the same unit as the bit-field before */
                } else {
                    off = al > 1 ? ir_binop_imm(F, IR_AND, IR_I64, ir_binop_imm(F, IR_ADD, IR_I64, cur, (long)al - 1), -(long)al) : cur;
                }
                store_value(u, addr_local(m->rt_slot), off);
                if (m->bits >= 0) {
                    long used = r->packed ? (long)(m->bit_offset + (unsigned)m->bits + 7) / 8 : (long)m->unit;
                    int end = ir_binop_imm(F, IR_ADD, IR_I64, off, used);
                    /* the furthest a bit-field of the unit reaches */
                    int later = ir_icmp(F, IR_UGT, IR_I64, end, cur);
                    int next = ir_vreg(F, IR_I64);
                    int b1 = ir_block(F), b2 = ir_block(F), join = ir_block(F);
                    ir_condbr(F, later, b1, b2);
                    ir_place(F, b1);
                    ir_mov(F, next, end);
                    ir_br(F, join);
                    ir_place(F, b2);
                    ir_mov(F, next, cur);
                    ir_place(F, join);
                    cur = next;
                } else {
                    cur = ir_binop(F, IR_ADD, IR_I64, off, size_of(m->type));
                }
                prev = m;
                prev_off = off;
            }
            size = cur >= 0 ? cur : iconst(IR_I64, (long)r->size);
        }
        if (r->align > 1) {
            size = ir_binop_imm(F, IR_AND, IR_I64, ir_binop_imm(F, IR_ADD, IR_I64, size, (long)r->align - 1), -(long)r->align);
        }
        store_value(u, addr_local(r->size_slot), size);
    }
}

static void local_decl(AST *decl) {
    size_t i;
    vm_record_layout(decl->u.decl.specs);
    for (i = 0; i < decl->u.decl.declarators->len; i++) {
        AST *id = decl->u.decl.declarators->items[i];
        Symbol *s = id->sym;
        AST *init = id->u.init_decl.init;
        vla_eval(id->u.init_decl.declarator);
        if (s && s->kind == SYM_VARIABLE && s->type && type_is_vla(s->type) && codegen_sym_is_local(s)) {
            vla_alloc(s);
            continue;
        }
        if (!s || s->kind != SYM_VARIABLE || !codegen_sym_is_local(s) || !init || !s->type) {
            continue;
        }
        if (local_vreg(s) >= 0) {
            /* a scalar in a register: its initializer, out of any braces */
            AST *item = init;
            while (item->kind == N_INIT_LIST && item->u.list->len) {
                item = item->u.list->items[0];
                if (item->kind == N_DESIGNATED) {
                    item = item->u.desig.init;
                }
            }
            if (item->kind != N_INIT_LIST && lower_check_value_type(item->loc, s->type)) {
                ir_mov(F, local_vreg(s), expr(item));
            } else if (item->kind == N_INIT_LIST && lower_check_value_type(item->loc, s->type)) {
                /* `= {}` */
                ir_mov(F, local_vreg(s), type_is_floating(s->type) ? float_const(0, s->type) : iconst(ty(s->type), 0));
            }
            continue;
        }
        if (s->by_ref) {
            init_base = ir_load(F, IR_I64, addr_local(s->offset), 8, 0);    /* an over-aligned local */
            init_base_off = s->offset;
        }
        if (init->kind == N_INIT_LIST || (s->type->kind == TY_ARRAY && init->kind == N_STRING)) {
            /* members the initializer leaves out are zero */
            zero_bytes(init_addr(s->offset), type_size(s->type));
        }
        codegen_local_initializer(s->type, init, s->offset);
        init_base = -1;
    }
}

/* ------------------------------------------------------------------ */
/* statements                                                         */
/* ------------------------------------------------------------------ */

/* the case labels of a switch body, not looking into nested switches */
static void collect_cases(AST *s, SwitchGen *sw) {
    size_t i;
    if (!s) {
        return;
    }
    switch (s->kind) {
        case N_CASE:
            if (sw->ncases == sw->cap) {
                sw->cap = sw->cap ? sw->cap * 2 : 16;
                sw->cases = xrealloc(sw->cases, sw->cap * sizeof(*sw->cases));
            }
            sw->cases[sw->ncases].stmt = s;
            sw->cases[sw->ncases].block = ir_block(F);
            sw->ncases++;
            collect_cases(s->u.stmt.body, sw);
            break;
        case N_DEFAULT:
            sw->default_block = ir_block(F);
            collect_cases(s->u.stmt.body, sw);
            break;
        case N_COMPOUND:
            for (i = 0; i < s->u.block.items->len; i++) {
                collect_cases(s->u.block.items->items[i], sw);
            }
            break;
        case N_IF:
            collect_cases(s->u.cond.then, sw);
            collect_cases(s->u.cond.otherwise, sw);
            break;
        case N_WHILE:
        case N_DO:
        case N_LABEL:
            collect_cases(s->u.stmt.body, sw);
            break;
        case N_FOR:
            collect_cases(s->u.loop.body, sw);
            break;
        default:
            break;
    }
}

static int case_block(AST *stmt_) {
    size_t i;
    for (i = 0; cur_switch && i < cur_switch->ncases; i++) {
        if (cur_switch->cases[i].stmt == stmt_) {
            return cur_switch->cases[i].block;
        }
    }
    diag_error(stmt_->loc, "internal error: case label not found");
    return ir_block(F);
}

static int label_block(Symbol *s) {
    if (!s->label) {
        s->label = ir_block(F) + 1;
    }
    return s->label - 1;
}

/*
 * GNU label addresses. Each label of the function whose address is taken
 * (in its code or in a static initializer) gets its block now, marked
 * with the label's assembler name, and a `goto *p` may go to any of them.
 * The optimizer leaves such a function alone.
 */
static int *addr_label_blocks = NULL;
static int naddr_labels = 0;
static int asm_goto_seen = 0;   /* the function has an asm goto: the optimizer leaves it alone */

static void addr_labels_begin(Scope *scope) {
    Symbol *s;
    naddr_labels = 0;
    asm_goto_seen = 0;
    nested_call_seen = 0;
    for (s = scope ? scope->first : NULL; s; s = s->next) {
        if (s->ns == NS_LABEL && s->addr_taken) {
            int b = label_block(s);
            F->blocks[b].addr_label = codegen_label_id(s);
            addr_label_blocks = xrealloc(addr_label_blocks, (size_t)(naddr_labels + 1) * sizeof(int));
            addr_label_blocks[naddr_labels++] = b;
        }
    }
}

/* ------------------------------------------------------------------ */
/* asm statements (see IRAsm in src/ir.h)                              */
/* ------------------------------------------------------------------ */

static int asm_value_size(Type *t) {
    if (type_is_floating(t) && !type_is_complex(t) && t->kind != TY_FLOAT128) {
        return (int)type_size(t);     /* a long double too, for the x87 constraints */
    }
    return is_value_type(t) && !type_is_complex(t) && !type_is_wide(t) && type_size(t) <= 8 ? (int)type_size(t) : 0;
}

/* the index of the operand named `name` (len bytes), or -1 */
static int asm_named(const IRAsm *a, const char *name, size_t len) {
    int k;
    for (k = 0; k < a->nops; k++) {
        if (a->ops[k].name && strlen(a->ops[k].name) == len && strncmp(a->ops[k].name, name, len) == 0) {
            return k;
        }
    }
    return -1;
}

static void asm_stmt(AST *s) {
    IRAsm a;
    ASTList *outs = s->u.asmst.outputs;
    ASTList *ins = s->u.asmst.inputs;
    int nouts = outs ? (int)outs->len : 0;
    int nins = ins ? (int)ins->len : 0;
    int *lv_addr = NULL;        /* outputs in registers: where the value goes after, from lvalue_addr */
    int *args = NULL;
    Type **arg_types = NULL;
    int nargs = 0;
    int k, idx;
    Type *vp = type_pointer(type_basic(TY_VOID));

    memset(&a, 0, sizeof(a));
    if (s->u.asmst.text) {
        char *text = sema_asm_text(s->u.asmst.text, s->loc);
        if (!text) {
            return;
        }
        a.text = ir_name(F, text);
        free(text);
        a.basic = 1;
    } else {
        char *text = NULL;
        sema_string_bytes(s->u.asmst.tmpl->u.text, &text);
        a.text = ir_name(F, text);
        free(text);
    }
    {
        const char *p = a.text;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p) {
            codegen_user_asm = 1;
        }
    }
    a.nops = nouts + nins;
    a.ops = a.nops ? xcalloc((size_t)a.nops, sizeof(IRAsmOp)) : NULL;
    args = xmalloc((size_t)(2 * a.nops + 1) * sizeof(int));
    arg_types = xmalloc((size_t)(2 * a.nops + 1) * sizeof(Type *));
    lv_addr = xmalloc((size_t)(nouts + 1) * sizeof(int));
    for (k = 0; k < a.nops; k++) {
        AST *op = k < nouts ? outs->items[k] : ins->items[k - nouts];
        IRAsmOp *o = &a.ops[k];
        o->match = -1;
        o->name = op->u.asmop.name ? ir_name(F, op->u.asmop.name) : NULL;
    }
    /* the outputs: their lvalues first, then what an in-out one holds */
    for (k = 0; k < nouts; k++) {
        AST *op = outs->items[k];
        AST *e = op->u.asmop.expr;
        IRAsmOp *o = &a.ops[k];
        AsmConstraint c;
        char *text = NULL;
        int size = asm_value_size(e->type);
        sema_string_bytes(op->u.asmop.constraint->u.text, &text);
        sema_asm_constraint(text, &c);
        free(text);
        o->is_output = 1;
        o->is_input = (char)c.inout;
        o->size = size;
        lv_addr[k] = -1;
        if (c.reg && size && !sema_is_bitfield(e) && (size <= 8 || c.specific == 't' || c.specific == 'u')) {
            o->kind = 'r';
            o->specific = c.specific;
            o->fp = (char)(c.fp && type_is_floating(e->type));
            o->slot = alloc_slot(16, 16);
            lv_addr[k] = lvalue_addr(e);
            if (c.inout) {
                store_value(type_unqualified(e->type), addr_local(o->slot), lvalue_load(e, lv_addr[k]));
            }
        } else {
            int ad = addr(e);
            o->kind = 'm';
            o->slot = alloc_slot(16, 16);
            store_value(vp, addr_local(o->slot), ad);
            args[nargs] = ad;
            arg_types[nargs++] = vp;
        }
        args[nargs] = addr_local(o->slot);
        arg_types[nargs++] = vp;
    }
    /* the inputs */
    for (k = 0; k < nins; k++) {
        AST *op = ins->items[k];
        AST *e = op->u.asmop.expr;
        IRAsmOp *o = &a.ops[nouts + k];
        AsmConstraint c;
        char *text = NULL;
        long cv;
        int size = asm_value_size(e->type);
        sema_string_bytes(op->u.asmop.constraint->u.text, &text);
        sema_asm_constraint(text, &c);
        free(text);
        o->is_input = 1;
        o->size = size;
        if (c.match_name) {
            c.match = asm_named(&a, c.match_name, c.match_len);
        }
        if (c.imm && type_is_integer(e->type) && sema_const_eval(e, &cv) && c.match < 0) {
            o->kind = 'i';
            o->imm = sema_fit(cv, e->type);
            continue;
        }
        if ((c.reg || c.match >= 0) && size && (size <= 8 || c.specific == 't' || c.specific == 'u'
                || (c.match >= 0 && c.match < nouts && (a.ops[c.match].specific == 't' || a.ops[c.match].specific == 'u')))) {
            Type *t = type_unqualified(e->type);
            o->kind = 'r';
            o->specific = c.specific;
            o->fp = (char)(c.fp && type_is_floating(t));
            o->match = c.match;
            if (c.match >= 0 && c.match < nouts) {
                o->fp = a.ops[c.match].fp;
                o->size = a.ops[c.match].size;
                if (a.ops[c.match].kind != 'r') {
                    o->kind = 'm';     /* tied to an output in memory: the same memory */
                    o->slot = a.ops[c.match].slot;
                    continue;
                }
            }
            o->slot = alloc_slot(16, 16);
            store_value(t, addr_local(o->slot), expr(e));
        } else {
            /* memory: the object, or a temporary holding the value */
            int ad;
            if (e->lvalue > 0) {
                ad = addr(e);
            } else {
                long tmp = alloc_slot(type_size(e->type) ? type_size(e->type) : 1, 16);
                ad = addr_local(tmp);
                if (by_address(e->type)) {
                    copy_bytes(ad, expr(e), type_size(e->type));
                } else {
                    store_value(type_unqualified(e->type), ad, expr(e));
                }
            }
            o->kind = 'm';
            o->slot = alloc_slot(16, 16);
            store_value(vp, addr_local(o->slot), ad);
            args[nargs] = ad;
            arg_types[nargs++] = vp;
        }
        args[nargs] = addr_local(o->slot);
        arg_types[nargs++] = vp;
    }
    if (s->u.asmst.clobbers) {
        a.nclobbers = (int)s->u.asmst.clobbers->len;
        a.clobbers = xmalloc((size_t)a.nclobbers * sizeof(char *));
        for (k = 0; k < a.nclobbers; k++) {
            char *text = NULL;
            sema_string_bytes(s->u.asmst.clobbers->items[k]->u.text, &text);
            a.clobbers[k] = ir_name(F, text);
            free(text);
        }
    }
    a.nlabels = s->u.asmst.labels ? (int)s->u.asmst.labels->len : 0;

    idx = ir_add_asm(F, &a);
    emit_call(ir_name(F, "__jcc_asm"), -1, nargs, args, arg_types, NULL, (size_t)nargs, 0, type_basic(TY_VOID), -1, -1);
    F->calls[F->ncalls - 1].asm_index = idx + 1;
    if (a.nlabels) {
        /* asm goto: the asm may jump to its labels, else it goes on */
        int *targets = xmalloc((size_t)(a.nlabels + 1) * sizeof(int));
        int next = ir_block(F);
        IRInst *i;
        targets[0] = next;
        for (k = 0; k < a.nlabels; k++) {
            Symbol *l = s->u.asmst.labels->items[k]->sym;
            targets[k + 1] = l ? label_block(l) : next;
        }
        i = ir_inst(F, ir_emit(F, IR_ASMBR, IR_VOID, -1, -1, -1));
        i->imm = ir_add_switch(F, targets, a.nlabels + 1);
        free(targets);
        ir_place(F, next);
        asm_goto_seen = 1;
    }
    /* the outputs in registers, to their lvalues */
    for (k = 0; k < nouts; k++) {
        AST *e = outs->items[k]->u.asmop.expr;
        if (a.ops[k].kind == 'r') {
            Type *t = type_unqualified(e->type);
            lvalue_store(e, lv_addr[k], load_value(e->loc, t, addr_local(a.ops[k].slot)));
        }
    }
    free(lv_addr);
    free(args);
    free(arg_types);
}

/*
 * The functions nested directly in this one: it keeps the optimizer away,
 * and a nested one whose address is taken gets its trampoline here, in
 * this frame, made at entry: code that loads the static chain (this frame)
 * and jumps to the function. x86_64 only, and the stack must then be
 * executable (codegen_exec_stack).
 */
static void nested_trampoline(Symbol *s) {
    static const unsigned char head[] = { 0x49, 0xbb };     /* movabs $chain, %r11 */
    static const unsigned char mid[] = { 0x49, 0xba };      /* movabs $fn, %r10 */
    static const unsigned char tail[] = { 0x41, 0xff, 0xe2, 0x90 };   /* jmp *%r10; nop */
    Type *u16 = type_basic(TY_USHORT);
    Type *u32 = type_basic(TY_UINT);
    Type *vp = type_pointer(type_basic(TY_VOID));
    long t;
    if (strcmp(arch_info->name, "x86_64") != 0) {
        return;     /* no executable stack (Apple arm64): the address cannot be taken (see addr) */
    }
    t = alloc_slot(32, 16);
    s->tramp_off = t;
    store_value(u16, addr_local(t), iconst(IR_I32, head[0] | head[1] << 8));
    store_value(vp, addr_local(t + 2), addr_local(0));
    store_value(u16, addr_local(t + 10), iconst(IR_I32, mid[0] | mid[1] << 8));
    store_value(vp, addr_local(t + 12), addr_global(s));
    store_value(u32, addr_local(t + 20), iconst(IR_I32, (long)((unsigned long)tail[0] | (unsigned long)tail[1] << 8
                                                                 | (unsigned long)tail[2] << 16 | (unsigned long)tail[3] << 24)));
    codegen_exec_stack = 1;
}

static void nested_functions_begin(AST *n) {
    size_t i;
    if (!n) {
        return;
    }
    switch (n->kind) {
        case N_FUNCTION_DEF:
            nested_call_seen = 1;
            if (n->sym && n->sym->addr_taken && n->sym->used) {
                nested_trampoline(n->sym);
            }
            return;     /* (its own nested ones are its business) */
        case N_COMPOUND:
            for (i = 0; i < n->u.block.items->len; i++) {
                nested_functions_begin(n->u.block.items->items[i]);
            }
            return;
        case N_IF:
            nested_functions_begin(n->u.cond.then);
            nested_functions_begin(n->u.cond.otherwise);
            return;
        case N_FOR:
            nested_functions_begin(n->u.loop.body);
            return;
        case N_LABEL: case N_CASE: case N_DEFAULT: case N_SWITCH: case N_WHILE: case N_DO:
            nested_functions_begin(n->u.stmt.body);
            return;
        default:
            return;
    }
}

static void computed_goto(AST *s) {
    int v = expr(s->u.stmt.expr);
    IRInst *i;
    if (!naddr_labels) {
        ir_emit(F, IR_UNREACHABLE, IR_VOID, -1, -1, -1);    /* no label's address is taken: nowhere to go */
        return;
    }
    i = ir_inst(F, ir_emit(F, IR_IJMP, IR_VOID, -1, v, -1));
    i->imm = ir_add_switch(F, addr_label_blocks, naddr_labels);
}

/*
 * At -O1 a switch whose case values are dense enough becomes a jump
 * table: the value less the smallest case indexes a table of blocks, and
 * values outside it go to the default. Returns 0 (and emits nothing)
 * when the cases are too few or too spread out.
 */
static int jump_table(SwitchGen *sw, Type *t, int v, int deflt) {
    long lo = 0, hi = 0, cv;
    unsigned long range;
    size_t i;
    int *targets;
    int idx;
    IRInst *ins;

    if (sw->ncases < 4) {
        return 0;
    }
    for (i = 0; i < sw->ncases; i++) {
        cv = 0;
        sema_const_eval(sw->cases[i].stmt->u.stmt.expr, &cv);
        cv = sema_fit(cv, t);
        if (i == 0 || cv < lo) {
            lo = cv;
        }
        if (i == 0 || cv > hi) {
            hi = cv;
        }
    }
    range = (unsigned long)hi - (unsigned long)lo + 1;
    if (range > 4096 || range > 3 * (unsigned long)sw->ncases + 4) {
        return 0;
    }
    targets = xmalloc(range * sizeof(int));
    for (i = 0; i < range; i++) {
        targets[i] = deflt;
    }
    for (i = 0; i < sw->ncases; i++) {
        cv = 0;
        sema_const_eval(sw->cases[i].stmt->u.stmt.expr, &cv);
        targets[(unsigned long)sema_fit(cv, t) - (unsigned long)lo] = sw->cases[i].block;
    }
    /* the index, as a 64 bit value: extended by the type's signedness, less the smallest case */
    if (ty(t) == IR_I32) {
        int w = ir_vreg(F, IR_I64);
        ins = ir_inst(F, ir_emit(F, type_is_unsigned(t) ? IR_ZEXT : IR_SEXT, IR_I64, w, v, -1));
        ins->size = 4;
        v = w;
    }
    idx = lo ? ir_binop_imm(F, IR_ADD, IR_I64, v, (long)(0 - (unsigned long)lo)) : v;
    ins = ir_inst(F, ir_emit(F, IR_SWITCH, IR_I64, -1, idx, -1));
    ins->aux = deflt;
    ins->imm = ir_add_switch(F, targets, (int)range);
    free(targets);
    return 1;
}

static void switch_stmt(AST *s) {
    SwitchGen sw;
    SwitchGen *outer = cur_switch;
    int saved_break = break_block;
    Type *t = s->u.stmt.expr->type;
    size_t i;
    int v;

    memset(&sw, 0, sizeof(sw));
    sw.default_block = -1;
    collect_cases(s->u.stmt.body, &sw);
    break_block = ir_block(F);
    v = expr(s->u.stmt.expr);
    if (!lower_check_value_type(s->u.stmt.expr->loc, t)) {
        v = iconst(IR_I32, 0);
    }
    if (codegen_opt_level > 0 && !type_is_wide(t) && jump_table(&sw, t, v, sw.default_block >= 0 ? sw.default_block : break_block)) {
        i = sw.ncases;      /* the table holds every case */
    } else {
        i = 0;
    }
    for (; i < sw.ncases; i++) {
        long cv = 0;
        int next = ir_block(F);
        int c;
        sema_const_eval(sw.cases[i].stmt->u.stmt.expr, &cv);
        if (type_is_wide(t)) {
            c = ir_icmp(F, IR_EQ, IR_I128, v, iconst(IR_I128, sema_fit(cv, t)));
        } else {
            c = ir_icmp_imm(F, IR_EQ, ty(t), v, sema_fit(cv, t));
        }
        ir_condbr(F, c, sw.cases[i].block, next);
        ir_place(F, next);
    }
    ir_br(F, sw.default_block >= 0 ? sw.default_block : break_block);
    cur_switch = &sw;
    stmt(s->u.stmt.body);
    cur_switch = outer;
    ir_place(F, break_block);
    break_block = saved_break;
    free(sw.cases);
}

/* the nodes of an expression, or a large number when it holds a statement expression or anything not simply an expression */
static int expr_size(const AST *e) {
    size_t i;
    int n;
    if (!e) {
        return 0;
    }
    switch (e->kind) {
        case N_IDENT: case N_INT: case N_FLOAT: case N_CHAR: case N_STRING: case N_SIZEOF_TYPE: case N_OFFSETOF:
            return 1;
        case N_UNARY:
            return 1 + expr_size(e->u.unary.operand);
        case N_BINARY:
        case N_ASSIGN:
            return 1 + expr_size(e->u.binary.left) + expr_size(e->u.binary.right);
        case N_COND:
            return 1 + expr_size(e->u.cond.cond) + expr_size(e->u.cond.then) + expr_size(e->u.cond.otherwise);
        case N_CALL:
            n = 4 + expr_size(e->u.call.func);
            for (i = 0; i < e->u.call.args->len; i++) {
                n += expr_size(e->u.call.args->items[i]);
            }
            return n;
        case N_INDEX:
            return 1 + expr_size(e->u.index.array) + expr_size(e->u.index.index);
        case N_MEMBER:
            return 1 + expr_size(e->u.member.object);
        case N_CAST:
        case N_IMPLICIT_CAST:
        case N_SIZEOF_EXPR:
            return e->u.cast.expr && e->u.cast.expr->kind == N_INIT_LIST ? 1000 : 1 + expr_size(e->u.cast.expr);
        default:
            return 1000;
    }
}

/* at -O1 a loop is rotated, its condition lowered twice, when the condition is small and simple */
static int rotate(const AST *cond) {
    return codegen_opt_level > 0 && expr_size(cond) <= 40;
}

static void return_stmt(AST *s) {
    AST *e = s->u.stmt.expr;
    IRInst *i;
    if (inline_ctx) {
        /* in a function inlined at its call: the result, then on after the call */
        InlineCtx *c = inline_ctx;
        if (e) {
            int v = expr(e);
            if (c->result >= 0 && v >= 0) {
                if (by_address(c->ret)) {
                    copy_object(c->result, v, c->ret);
                } else {
                    ir_mov(F, c->result, convert(v, e->type, c->ret));
                }
            }
        }
        ir_br(F, c->end);
        return;
    }
    if (!e) {
        ir_emit(F, IR_RET, IR_VOID, -1, -1, -1);
        return;
    }
    if (!by_address(e->type)) {
        int v = expr(e);
        if (v < 0) {
            ir_emit(F, IR_RET, IR_VOID, -1, -1, -1);
            return;
        }
        ir_emit(F, IR_RET, ty(e->type), -1, v, -1);
        return;
    }
    if (sret_off) {
        /* copy into the caller's object; the callee returns its address */
        int p = ir_load(F, IR_I64, addr_local(sret_off), 8, 0);
        copy_bytes(p, expr(e), type_size(e->type));
        ir_emit(F, IR_RET, IR_I64, -1, p, -1);
        return;
    }
    {
        ArgType at;
        int a = expr(e);
        codegen_arg_type(e->type, &at);
        i = ir_inst(F, ir_emit(F, IR_RET, IR_I64, -1, a, -1));
        i->flags |= IRF_RECORD;
        i->imm = ir_add_argtype(F, &at);
    }
}

static void stmt(AST *s) {
    size_t i;
    int b_body, b_else, b_end, b_cond, b_step;
    int saved_break;
    int saved_continue;

    if (!s) {
        return;
    }
    switch (s->kind) {
        case N_COMPOUND:
            for (i = 0; i < s->u.block.items->len; i++) {
                AST *item = s->u.block.items->items[i];
                if (item->kind == N_DECLARATION) {
                    local_decl(item);
                } else {
                    stmt(item);
                }
            }
            break;

        case N_EXPR_STMT:
            if (s->u.stmt.expr) {
                expr(s->u.stmt.expr);
            }
            break;

        case N_IF:
            b_body = ir_block(F);
            b_else = ir_block(F);
            b_end = s->u.cond.otherwise ? ir_block(F) : b_else;
            branch(s->u.cond.cond, b_body, b_else);
            ir_place(F, b_body);
            stmt(s->u.cond.then);
            if (s->u.cond.otherwise) {
                jump(b_end);
                ir_place(F, b_else);
                stmt(s->u.cond.otherwise);
            }
            ir_place(F, b_end);
            break;

        case N_WHILE:
            saved_break = break_block;
            saved_continue = continue_block;
            continue_block = ir_block(F);
            break_block = ir_block(F);
            b_body = ir_block(F);
            if (rotate(s->u.stmt.expr)) {
                /* -O1: the test before the loop and again at the bottom, so that each trip takes one branch */
                branch(s->u.stmt.expr, b_body, break_block);
                ir_place(F, b_body);
                stmt(s->u.stmt.body);
                ir_place(F, continue_block);
                branch(s->u.stmt.expr, b_body, break_block);
            } else {
                ir_place(F, continue_block);
                branch(s->u.stmt.expr, b_body, break_block);
                ir_place(F, b_body);
                stmt(s->u.stmt.body);
                jump(continue_block);
            }
            ir_place(F, break_block);
            break_block = saved_break;
            continue_block = saved_continue;
            break;

        case N_DO:
            saved_break = break_block;
            saved_continue = continue_block;
            b_body = ir_block(F);
            continue_block = ir_block(F);
            break_block = ir_block(F);
            ir_place(F, b_body);
            stmt(s->u.stmt.body);
            ir_place(F, continue_block);
            branch(s->u.stmt.expr, b_body, break_block);
            ir_place(F, break_block);
            break_block = saved_break;
            continue_block = saved_continue;
            break;

        case N_FOR:
            saved_break = break_block;
            saved_continue = continue_block;
            b_cond = ir_block(F);
            b_body = ir_block(F);
            b_step = ir_block(F);
            break_block = ir_block(F);
            continue_block = b_step;
            if (s->u.loop.init) {
                expr(s->u.loop.init);
            }
            if (s->u.loop.cond && rotate(s->u.loop.cond)) {
                /* -O1: rotated as a while loop is */
                branch(s->u.loop.cond, b_body, break_block);
                ir_place(F, b_body);
                stmt(s->u.loop.body);
                ir_place(F, b_step);
                if (s->u.loop.step) {
                    expr(s->u.loop.step);
                }
                branch(s->u.loop.cond, b_body, break_block);
                ir_place(F, b_cond);    /* unused */
                ir_br(F, break_block);
                ir_place(F, break_block);
                break_block = saved_break;
                continue_block = saved_continue;
                break;
            }
            ir_place(F, b_cond);
            if (s->u.loop.cond) {
                branch(s->u.loop.cond, b_body, break_block);
            }
            ir_place(F, b_body);
            stmt(s->u.loop.body);
            ir_place(F, b_step);
            if (s->u.loop.step) {
                expr(s->u.loop.step);
            }
            jump(b_cond);
            ir_place(F, break_block);
            break_block = saved_break;
            continue_block = saved_continue;
            break;

        case N_SWITCH:
            switch_stmt(s);
            break;

        case N_CASE:
            ir_place(F, case_block(s));
            stmt(s->u.stmt.body);
            break;

        case N_DEFAULT:
            if (cur_switch && cur_switch->default_block >= 0) {
                ir_place(F, cur_switch->default_block);
            }
            stmt(s->u.stmt.body);
            break;

        case N_LABEL:
            if (s->sym) {
                ir_place(F, label_block(s->sym));
            }
            stmt(s->u.stmt.body);
            break;

        case N_ASM_STMT:
            asm_stmt(s);
            break;

        case N_FUNCTION_DEF:
            break;      /* a GNU nested function: lowered on its own, after this one (src/codegen.c) */

        case N_GOTO:
            if (s->u.stmt.expr) {
                computed_goto(s);
            } else if (s->sym && s->sym->owner_fn && s->sym->owner_fn != cur_fsym) {
                /* out of this nested function, to its label in the enclosing function: that frame, its stack pointer */
                Symbol *o = s->sym->owner_fn;
                int fp = frame_of(o);
                int target = ir_vreg(F, IR_I64);
                IRInst *i = ir_inst(F, ir_emit(F, IR_LABEL_ADDR, IR_I64, target, -1, -1));
                i->imm = codegen_label_id(s->sym);
                i = ir_inst(F, ir_emit(F, IR_NLGOTO, IR_VOID, -1, fp, target));
                i->imm = o->nl_sp_off;
                F->noopt = 1;
                nested_call_seen = 1;
            } else if (s->sym) {
                jump(label_block(s->sym));
            }
            break;

        case N_BREAK:
            if (break_block >= 0) {
                jump(break_block);
            }
            break;

        case N_CONTINUE:
            if (continue_block >= 0) {
                jump(continue_block);
            }
            break;

        case N_RETURN:
            return_stmt(s);
            break;

        default:
            diag_error(s->loc, "internal error: %s is not a statement", node_kind_name(s->kind));
            break;
    }
}

/* ------------------------------------------------------------------ */
/* functions                                                          */
/* ------------------------------------------------------------------ */

static long alloc_slot(size_t size, size_t align) {
    if (size == 0) {
        size = 1;
    }
    if (align == 0) {
        align = 1;
    }
    frame_off -= (long)size;
    frame_off &= -(long)align;
    return frame_off;
}

static void alloc_local(Symbol *s) {
    s->offset = alloc_slot(type_size(s->type), type_align(s->type));
}

/*
 * A local aligned past the 16 bytes every target keeps its frame to
 * (__attribute__((aligned(32))), say): a frame offset cannot promise it,
 * so the object goes in a slot with room to spare and its address,
 * rounded up at function entry, in a slot of its own; it is reached
 * through that address, as a VLA is (Symbol.by_ref).
 */
/* room for `size` bytes at `align` (past 16), placed at function entry: the slot holding their address; *aligned_out, the address */
static long overaligned_slot(size_t size, size_t align, int *aligned_out) {
    long raw = alloc_slot(size + align - 16, 16);
    long ptr = alloc_slot(8, 8);
    int a = ir_binop_imm(F, IR_ADD, IR_I64, addr_local(raw), (long)align - 1);
    a = ir_binop_imm(F, IR_AND, IR_I64, a, -(long)align);
    ir_store(F, IR_I64, addr_local(ptr), a, 8);
    if (aligned_out) {
        *aligned_out = a;
    }
    return ptr;
}

static void overaligned_local(Symbol *s) {
    s->by_ref = 1;
    s->offset = overaligned_slot(type_size(s->type), type_align(s->type), NULL);
}

/* a temporary of `size` bytes for the node `e`, at `align`; one aligned past 16 is reached through its address, as an over-aligned local is */
static void alloc_temp(AST *e, size_t size, size_t align) {
    if (align > 16) {
        e->temp = overaligned_slot(size, align, NULL);
        e->temp_by_ref = 1;
    } else {
        e->temp = alloc_slot(size, align);
        e->temp_by_ref = 0;
    }
}

/* the address of e's temporary */
static int temp_addr(const AST *e) {
    return e->temp_by_ref ? ir_load(F, IR_I64, addr_local(e->temp), 8, 0) : addr_local(e->temp);
}

/* can this local live in a register: a scalar whose address is never taken, in a function without setjmp */
static int promotable(const Symbol *s) {
    if (all_in_memory || s->vreg == -1 || s->by_ref || !s->type || s->captured) {
        return 0;
    }
    if (s->type->quals & TQ_VOLATILE) {
        return 0;
    }
    if (!is_value_type(s->type)) {
        return 0;
    }
    if (s->type->kind == TY_FLOAT16 || s->type->kind == TY_FLOAT128 || (s->type->kind == TY_LDOUBLE && arch_info->ldouble_size > 8 && !arch_info->ldouble_x87)) {
        return 0;
    }
    return 1;
}

/* the object an lvalue expression is part of, when it is a local: it must stay in memory */
static void mark_base(AST *e) {
    for (;;) {
        switch (e->kind) {
            case N_IMPLICIT_CAST:
            case N_CAST:
                e = e->u.cast.expr;
                continue;
            case N_MEMBER:
                if (e->u.member.arrow) {
                    return;
                }
                e = e->u.member.object;
                continue;
            case N_INDEX:
                if (type_is_pointer(e->u.index.array->type)) {
                    return;
                }
                e = e->u.index.array;
                continue;
            case N_UNARY:
                if (e->u.unary.op == OP_REAL || e->u.unary.op == OP_IMAG) {
                    e = e->u.unary.operand;
                    continue;
                }
                return;
            case N_IDENT:
                if (e->sym && (e->sym->kind == SYM_VARIABLE || e->sym->kind == SYM_PARAM)) {
                    e->sym->vreg = -1;
                }
                return;
            default:
                return;
        }
    }
}

static int is_setjmp(const char *name) {
    return strcmp(name, "setjmp") == 0 || strcmp(name, "_setjmp") == 0 || strcmp(name, "sigsetjmp") == 0
        || strcmp(name, "__sigsetjmp") == 0 || strcmp(name, "__builtin_setjmp") == 0 || strcmp(name, "vfork") == 0;
}

/* find the locals whose address is taken (by &, or by the va_list builtins), and whether setjmp is called */
static void mark_memory(AST *n) {
    size_t i;
    if (!n) {
        return;
    }
    switch (n->kind) {
        case N_UNARY:
            if (n->u.unary.op == OP_ADDR) {
                mark_base(n->u.unary.operand);
            }
            mark_memory(n->u.unary.operand);
            break;
        case N_BINARY:
        case N_ASSIGN:
            mark_memory(n->u.binary.left);
            mark_memory(n->u.binary.right);
            break;
        case N_COND:
        case N_IF:
            mark_memory(n->u.cond.cond);
            mark_memory(n->u.cond.then);
            mark_memory(n->u.cond.otherwise);
            break;
        case N_CALL: {
            Symbol *c = direct_callee(n->u.call.func);
            if (c) {
                if (is_setjmp(c->name)) {
                    all_in_memory = 1;
                }
                if (strcmp(c->name, "__builtin_va_start") == 0 || strcmp(c->name, "__builtin_va_end") == 0
                        || strcmp(c->name, "__builtin_va_copy") == 0) {
                    for (i = 0; i < n->u.call.args->len && i < 2; i++) {
                        mark_base(n->u.call.args->items[i]);
                    }
                }
            }
            mark_memory(n->u.call.func);
            for (i = 0; i < n->u.call.args->len; i++) {
                mark_memory(n->u.call.args->items[i]);
            }
            break;
        }
        case N_INDEX:
            mark_memory(n->u.index.array);
            mark_memory(n->u.index.index);
            break;
        case N_MEMBER:
            mark_memory(n->u.member.object);
            break;
        case N_VA_ARG:
            mark_base(n->u.cast.expr);
            mark_memory(n->u.cast.expr);
            break;
        case N_CAST:
        case N_IMPLICIT_CAST:
            mark_memory(n->u.cast.expr);
            break;
        case N_INIT_LIST:
            for (i = 0; i < n->u.list->len; i++) {
                mark_memory(n->u.list->items[i]);
            }
            break;
        case N_DESIGNATED:
            mark_memory(n->u.desig.init);
            break;
        case N_COMPOUND_LIT:
            mark_memory(n->u.cast.expr->u.init_decl.init);
            break;
        case N_STMT_EXPR:
            mark_memory(n->u.stmt.body);
            break;
        case N_COMPOUND:
            for (i = 0; i < n->u.block.items->len; i++) {
                mark_memory(n->u.block.items->items[i]);
            }
            break;
        case N_DECLARATION:
            for (i = 0; i < n->u.decl.declarators->len; i++) {
                mark_memory(n->u.decl.declarators->items[i]->u.init_decl.init);
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
        case N_GOTO:
            mark_memory(n->u.stmt.expr);
            mark_memory(n->u.stmt.body);
            break;
        case N_ASM_STMT: {
            /* an operand the asm reads or writes in memory: its object stays there */
            ASTList *lists[2];
            size_t k;
            lists[0] = n->u.asmst.outputs;
            lists[1] = n->u.asmst.inputs;
            for (k = 0; k < 2; k++) {
                for (i = 0; lists[k] && i < lists[k]->len; i++) {
                    AST *op = lists[k]->items[i];
                    AsmConstraint c;
                    char *text = NULL;
                    sema_string_bytes(op->u.asmop.constraint->u.text, &text);
                    sema_asm_constraint(text, &c);
                    free(text);
                    if (c.mem && !(c.reg || (c.imm && k == 1))) {
                        mark_base(op->u.asmop.expr);
                    }
                    mark_memory(op->u.asmop.expr);
                }
            }
            break;
        }
        case N_FOR:
            mark_memory(n->u.loop.init);
            mark_memory(n->u.loop.cond);
            mark_memory(n->u.loop.step);
            mark_memory(n->u.loop.body);
            break;
        default:
            break;
    }
}

/* a call returning a record needs a temporary for the result; complex nodes own slots */
static void assign_temps(AST *e) {
    size_t i;
    if (!e) {
        return;
    }
    if (e->type && type_is_complex(e->type) && e->kind != N_CALL && e->kind != N_VA_ARG && e->kind != N_COMPOUND_LIT) {
        /* every complex valued node: its result, or its consumer's working copy */
        e->temp = alloc_slot(type_size(e->type), 16);
    }
    switch (e->kind) {
        case N_CALL:
            assign_temps(e->u.call.func);
            for (i = 0; i < e->u.call.args->len; i++) {
                assign_temps(e->u.call.args->items[i]);
            }
            if (e->type && by_address(e->type)) {
                alloc_temp(e, (type_size(e->type) + 7) & ~(size_t)7, type_align(e->type) > 8 ? type_align(e->type) : 8);
            }
            break;
        case N_UNARY:
            assign_temps(e->u.unary.operand);
            break;
        case N_BINARY:
        case N_ASSIGN:
            assign_temps(e->u.binary.left);
            assign_temps(e->u.binary.right);
            break;
        case N_COND:
            assign_temps(e->u.cond.cond);
            assign_temps(e->u.cond.then);
            assign_temps(e->u.cond.otherwise);
            break;
        case N_INDEX:
            assign_temps(e->u.index.array);
            assign_temps(e->u.index.index);
            break;
        case N_MEMBER:
            assign_temps(e->u.member.object);
            break;
        case N_VA_ARG:
            if (by_address(e->type)) {
                ArgType at;
                codegen_arg_type(e->type, &at);
                if (type_is_complex(e->type)) {
                    e->temp = alloc_slot(type_size(e->type) > 16 ? type_size(e->type) : 16, 16);
                } else if (at.sse) {
                    e->temp = alloc_slot(16, 16);
                }
            }
            assign_temps(e->u.cast.expr);
            break;
        case N_CAST:
        case N_IMPLICIT_CAST:
            if (e->kind == N_CAST && e->u.cast.type) {
                vla_slots(e->u.cast.type->u.typed.declarator);
            }
            assign_temps(e->u.cast.expr);
            break;
        case N_SIZEOF_TYPE:
            vla_slots(e->u.cast.type->u.typed.declarator);
            break;
        case N_INIT_LIST:
            for (i = 0; i < e->u.list->len; i++) {
                assign_temps(e->u.list->items[i]);
            }
            break;
        case N_DESIGNATED:
            assign_temps(e->u.desig.init);
            break;
        case N_COMPOUND_LIT:
            if (!e->sym) {
                alloc_temp(e, type_size(e->type), type_is_complex(e->type) ? 16 : type_align(e->type));
            }
            assign_temps(e->u.cast.expr->u.init_decl.init);
            break;
        case N_STMT_EXPR:
            assign_locals(e->u.stmt.body);
            break;
        default:
            break;
    }
}

/* give every local in a function body a register or a frame slot; static locals become data */
static void assign_locals(AST *s) {
    size_t i;
    if (!s) {
        return;
    }
    switch (s->kind) {
        case N_DECLARATION:
            vm_record_slots(s->u.decl.specs);
            for (i = 0; i < s->u.decl.declarators->len; i++) {
                AST *id = s->u.decl.declarators->items[i];
                Symbol *sym = id->sym;
                vla_slots(id->u.init_decl.declarator);
                if (!sym || sym->kind != SYM_VARIABLE || sym->decl != id || !sym->type) {
                    continue;
                }
                if (codegen_sym_is_static_local(sym)) {
                    codegen_static_local(sym);
                } else if (codegen_sym_is_local(sym)) {
                    if (type_is_vla(sym->type)) {
                        /* its address, then the stack pointer to return to (zero: not allocated yet) */
                        sym->vreg = 0;
                        sym->by_ref = 1;
                        sym->offset = alloc_slot(16, 8);
                        {
                            int addr = addr_local(sym->offset + 8);
                            ir_store(F, IR_I64, addr, iconst(IR_I64, 0), 8);
                        }
                    } else if (promotable(sym)) {
                        sym->vreg = ir_vreg_local(F, ty(sym->type), sym->name) + 1;
                        if (!id->u.init_decl.init && type_is_integer(sym->type) && type_size(sym->type) < 4) {
                            /*
                             * A char or short without an initializer: a frame slot would hold
                             * some byte, a register holds a whole word, so code that reads it
                             * before writing it (undefined, but gcc's tests do) would see a
                             * value out of the type's range. Zero is in range and costs one move.
                             */
                            ir_mov(F, sym->vreg - 1, iconst(IR_I32, 0));
                        }
                    } else if (type_align(sym->type) > 16) {
                        sym->vreg = 0;
                        overaligned_local(sym);
                    } else {
                        sym->vreg = 0;
                        alloc_local(sym);
                    }
                    assign_temps(id->u.init_decl.init);
                }
            }
            break;
        case N_COMPOUND:
            for (i = 0; i < s->u.block.items->len; i++) {
                assign_locals(s->u.block.items->items[i]);
            }
            break;
        case N_IF:
            assign_temps(s->u.cond.cond);
            assign_locals(s->u.cond.then);
            assign_locals(s->u.cond.otherwise);
            break;
        case N_WHILE:
        case N_DO:
        case N_SWITCH:
        case N_CASE:
        case N_DEFAULT:
        case N_LABEL:
            assign_temps(s->u.stmt.expr);
            assign_locals(s->u.stmt.body);
            break;
        case N_FOR:
            assign_temps(s->u.loop.init);
            assign_temps(s->u.loop.cond);
            assign_temps(s->u.loop.step);
            assign_locals(s->u.loop.body);
            break;
        case N_EXPR_STMT:
        case N_RETURN:
        case N_GOTO:
            assign_temps(s->u.stmt.expr);
            break;
        case N_ASM_STMT:
            for (i = 0; s->u.asmst.outputs && i < s->u.asmst.outputs->len; i++) {
                assign_temps(s->u.asmst.outputs->items[i]->u.asmop.expr);
            }
            for (i = 0; s->u.asmst.inputs && i < s->u.asmst.inputs->len; i++) {
                assign_temps(s->u.asmst.inputs->items[i]->u.asmop.expr);
            }
            break;
        default:
            break;
    }
}

IRFunc *lower_function(AST *fdef) {
    IRFunc *f;
    Symbol *s = fdef->sym;
    Type *t = fdef->type;
    AST *fn = ast_declarator_innermost(fdef->u.func.declarator);
    ASTList *params = fn ? fn->u.declarator.params : NULL;
    Symbol **psyms = NULL;
    IRParam *ps = NULL;
    ArgAlloc alloc;
    size_t nparams = 0;
    size_t i;
    int entry;

    if (!s || !t || !fn) {
        return NULL;
    }
    if (!check_arg_type(fdef->loc, t->base)) {
        return NULL;
    }
    if (params && params->len) {
        psyms = xrealloc(NULL, params->len * sizeof(*psyms));
        for (i = 0; i < params->len; i++) {
            Symbol *p = params->items[i]->sym;
            if (p && p->kind == SYM_PARAM && p->type) {
                psyms[nparams++] = p;
            }
        }
    }
    for (i = 0; i < nparams; i++) {
        if (!check_arg_type(fdef->loc, psyms[i]->type)) {
            free(psyms);
            return NULL;
        }
        psyms[i]->vreg = 0;
        psyms[i]->by_ref = 0;
    }

    F = ir_func_new(s->name);
    F->name = ir_name(F, codegen_sym_name(s));     /* its assembler name: an asm label's, or a nested function's */
    F->is_global = s->storage == STORAGE_STATIC ? 0 : s->weak ? 2 : 1;
    F->is_variadic = t->variadic;
    F->align = s->type && s->type->kind == TY_FUNC ? s->type->align : 0;
    F->is_main = strcmp(s->name, "main") == 0;
    F->opt_level = codegen_opt_level;
    frame_off = 0;
    sret_off = 0;
    all_in_memory = s->nonlocal_target;     /* a nested function may jump back into this frame: registers do not survive */
    cur_fsym = s;
    if (strcmp(arch_info->name, "x86_64") == 0 && calls_named(fdef->u.func.body, "__builtin_apply_args")) {
        F->apply_args_off = alloc_slot(192, 16);
    }
    if (s->parent_fn) {
        /* a GNU nested function: the prologue stores its static chain here */
        s->chain_off = alloc_slot(8, 8);
        F->chain_off = s->chain_off;
    }
    break_block = -1;
    continue_block = -1;
    cur_switch = NULL;
    entry = ir_block(F);
    ir_place(F, entry);

    /* which locals must stay in memory */
    mark_memory(fdef->u.func.body);

    /* where the parameters arrive, and where each lives */
    if (nparams) {
        ps = xrealloc(NULL, nparams * sizeof(*ps));
        memset(ps, 0, nparams * sizeof(*ps));
    }
    cg->args_begin(&alloc, nparams, returns_via_sret(t->base));
    for (i = 0; i < nparams; i++) {
        Symbol *p = psyms[i];
        IRParam *ip = &ps[i];
        Type *abi = p->type;
        ArgType at;
        /*
         * Without a prototype the caller applied the default argument
         * promotions, so a float arrives as a double and a char or short
         * as an int. Integers only need the low bytes, which are where
         * the parameter's own type reads them; a float has to be
         * converted on entry, from wherever the double landed.
         */
        if (!t->has_proto) {
            if (abi->kind == TY_FLOAT) {
                abi = type_basic(TY_DOUBLE);
            } else if (type_is_integer(abi)) {
                abi = type_promote(abi);
            }
        }
        codegen_arg_type(abi, &at);
        cg->args_next(&alloc, &at, &ip->loc);
        p->by_ref = ip->loc.by_ref;
        /* an integer promoted by a K&R call is stored and extended at its own width: the low bytes are the value */
        ip->size = type_size(p->type);
        ip->is_signed = type_is_integer(p->type) && !type_is_unsigned(p->type);
        ip->is_float = type_is_floating(abi);
        ip->wide = type_is_wide(abi);
        ip->by_addr = by_address(abi);
        ip->vreg = -1;
        ip->kr_from = 0;
        if (abi != p->type && abi->kind == TY_DOUBLE) {
            /* a K&R float: the double goes to a slot of its own (or sits in the caller's area), the float is made from it below */
            ip->kr_from = ip->loc.in_regs ? alloc_slot(8, 8) : cg->stack_param_offset(ip->loc.offset);
            ip->offset = ip->kr_from;
            if (promotable(p)) {
                p->vreg = ir_vreg_local(F, IR_F32, p->name) + 1;
            } else {
                alloc_local(p);
            }
        } else if (!ip->loc.in_regs) {
            p->offset = cg->stack_param_offset(ip->loc.offset);
            ip->offset = p->offset;
            if (promotable(p)) {
                p->vreg = ir_vreg_local(F, ty(p->type), p->name) + 1;
            }
        } else if (ip->loc.by_ref) {
            p->offset = alloc_slot(8, 8);
            ip->offset = p->offset;
        } else if (by_address(p->type)) {
            p->offset = alloc_slot(p->type->kind == TY_FLOAT128 ? 16 : 8 * ip->loc.nregs, type_align(p->type) > 8 ? type_align(p->type) : 8);
            ip->offset = p->offset;
        } else if (promotable(p)) {
            p->vreg = ir_vreg_local(F, ty(p->type), p->name) + 1;
            ip->vreg = p->vreg - 1;
        } else {
            alloc_local(p);
            ip->offset = p->offset;
        }
    }
    /* the incoming stack ends here: args_end adds the copies a caller makes of records passed by reference, which are its */
    F->nstack_params = alloc.nstack;
    {
        ArgLoc *locs = NULL;
        if (nparams) {
            locs = xrealloc(NULL, nparams * sizeof(*locs));
            for (i = 0; i < nparams; i++) {
                locs[i] = ps[i].loc;
            }
        }
        cg->args_end(&alloc, locs, nparams);
        free(locs);
    }
    if (returns_via_sret(t->base)) {
        sret_off = alloc_slot(8, 8);
    }
    F->sret_off = sret_off;
    F->nreg_params = alloc.nregs;
    F->nfreg_params = alloc.nfregs;
    for (i = 0; i < nparams; i++) {
        ir_param_add(F, &ps[i]);
    }
    if (F->apply_args_off) {
        /* where the stack arguments start, beside the registers the prologue saved */
        ir_store(F, IR_I64, addr_local(F->apply_args_off), addr_local(cg->stack_param_offset(0)), 8);
    }
    addr_labels_begin(fdef->u.func.scope);
    if (s->nonlocal_target) {
        /* a nested function jumps to a label here: the stack pointer is kept for it (and everything is in memory) */
        F->save_all = 1;
        s->nl_sp_off = alloc_slot(8, 8);
        {
            int sp = ir_vreg(F, IR_I64);
            ir_emit(F, IR_SPSAVE, IR_I64, sp, -1, -1);
            ir_store(F, IR_I64, addr_local(s->nl_sp_off), sp, 8);
        }
    }
    assign_locals(fdef->u.func.body);
    nested_functions_begin(fdef->u.func.body);

    /* the entry: parameters that the prologue cannot place itself */
    for (i = 0; i < nparams; i++) {
        Symbol *p = psyms[i];
        IRParam *ip = &F->params[i];
        if (ip->kr_from) {
            /* a K&R float parameter: the double it arrived as, converted */
            int d = ir_load(F, IR_F64, addr_local(ip->kr_from), 8, 0);
            int v = convert_inst(IR_FTOF, IR_F32, d, IR_F64, 0);
            if (local_vreg(p) >= 0) {
                ir_mov(F, local_vreg(p), v);
            } else {
                ir_store(F, IR_F32, addr_local(p->offset), v, 4);
            }
        } else if (!ip->loc.in_regs && local_vreg(p) >= 0) {
            /* a stack parameter kept in a register: loaded from the caller's area */
            ir_mov(F, local_vreg(p), load_value(fdef->loc, p->type, addr_local(ip->offset)));
        } else if (ip->loc.in_regs && !ip->loc.by_ref && local_vreg(p) < 0 && type_align(p->type) > 16) {
            /*
             * Aligned past the frame's 16 bytes: the prologue stored the registers in a slot
             * the frame cannot align, so the parameter moves to an over-aligned one (see
             * overaligned_local) and is reached through its address from here on.
             */
            int a;
            p->offset = overaligned_slot(type_size(p->type), type_align(p->type), &a);
            p->by_ref = 1;
            copy_bytes(a, addr_local(ip->offset), type_size(p->type));
        }
    }

    /* the sizes of variable length array parameters (`int a[n][m]`, adjusted to a pointer to a VLA) */
    for (i = 0; params && !fn->u.declarator.kr && i < params->len; i++) {
        AST *pd = params->items[i]->u.typed.declarator;
        vla_slots(pd);
        vla_eval(pd);
    }

    stmt(fdef->u.func.body);
    if (F->cur >= 0) {
        if (F->is_main) {
            ir_emit(F, IR_RET, IR_I32, -1, iconst(IR_I32, 0), -1);     /* falling off the end of main returns 0 */
        } else {
            ir_emit(F, IR_RET, IR_VOID, -1, -1, -1);
        }
    }
    F->locals_size = (-frame_off + 15) & ~15L;

    /* a function calling setjmp keeps every value as -O0 would, since longjmp restores registers to their old values */
    F->noopt = all_in_memory || naddr_labels > 0 || asm_goto_seen || nested_call_seen;
    free(psyms);
    free(ps);
    f = F;
    F = NULL;
    if (ir_verify(f) != 0) {
        ir_func_free(f);
        return NULL;
    }
    return f;
}

void lower_emit(IRFunc *f) {
    if (codegen_dump_ir) {
        ir_dump(f, stdout);
    }
    ir_count_uses(f);
    cg->select(f);
    ir_func_free(f);
}
