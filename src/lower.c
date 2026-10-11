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
    return type_is_record(t) || type_is_complex(t);
}

/* types whose values live in registers: scalars but complex ones, and a va_list that is a plain pointer */
static int is_value_type(const Type *t) {
    return (type_is_scalar(t) && !type_is_complex(t)) || (t->kind == TY_VA_LIST && !arch_info->va_list_is_array);
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
    if (t->kind == TY_CFLOAT || t->kind == TY_CDOUBLE || t->kind == TY_CLDOUBLE) {
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
    IRInst *i = ir_inst(F, ir_emit(F, IR_ADDR_GLOBAL, IR_I64, dst, -1, -1));
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

static void zero_bytes(int addr_v, size_t size) {
    IRInst *i = ir_inst(F, ir_emit(F, IR_ZERO, IR_VOID, -1, addr_v, -1));
    i->imm = (long)size;
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
static int truth(int v, const Type *t) {
    IRType it = ty(t);
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
static int convert(int v, Type *from, Type *to) {
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

/* a narrow integer result re-extended to its type's canonical form */
static int canon(int v, const Type *t) {
    size_t n;
    if (!type_is_integer(t) || t->kind == TY_BOOL) {
        return v;
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

static int load_value(Loc loc, Type *t, int a) {
    int v;
    if (!is_value_type(t) || !lower_check_value_type(loc, t)) {
        return a;       /* a record or array: its address is the value */
    }
    v = ir_load(F, ty(t), a, type_size(t), type_is_integer(t) && !type_is_unsigned(t));
    mark_volatile(t);
    return v;
}

static void store_value(Type *t, int a, int v) {
    ir_store(F, ty(t), a, v, type_size(t));
    mark_volatile(t);
}

/* `t` is the lvalue's type, which carries the qualifiers of the record it is a member of */
static int load_bits(Member *m, const Type *t, int a) {
    int dst = ir_vreg(F, ty(m->type));
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
    int dst = ir_vreg(F, ty(m->type));
    IRInst *i = ir_inst(F, ir_emit(F, IR_STOREBITS, ty(m->type), dst, a, v));
    i->size = (unsigned char)m->unit;
    i->imm = (long)m->bit_offset | ((long)m->bits << 8);
    if (!type_is_unsigned(m->type)) {
        i->flags |= IRF_SIGNED;
    }
    mark_volatile(t);
    return dst;
}

/* an lvalue's address, or -1 for a local that lives in a register */
static int lvalue_addr(AST *l) {
    if (l->kind == N_IDENT && l->sym && local_vreg(l->sym) >= 0) {
        return -1;
    }
    return addr(l);
}

/* the value of the lvalue `l`, whose address is `a` (-1: a register local) */
static int lvalue_load(AST *l, int a) {
    if (a < 0) {
        return local_vreg(l->sym);
    }
    if (sema_is_bitfield(l)) {
        return load_bits(l->u.member.member, l->type, a);
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

    if (!type_is_pointer(a->type)) {
        AST *t = a;
        a = i;
        i = t;
    }
    size = type_size(a->type->base);
    p = expr(a);
    if (sema_const_eval(i, &v)) {
        if (v == 0) {
            return p;
        }
        return ir_binop_imm(F, IR_ADD, IR_I64, p, (long)((unsigned long)v * size));
    }
    return ir_binop(F, IR_ADD, IR_I64, p, scaled_index(expr(i), i->type, size));
}

static int addr(AST *e) {
    switch (e->kind) {
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
                a = addr_local(s->offset);
                if (s->by_ref) {
                    a = ir_load(F, IR_I64, a, 8, 0);
                }
                return a;
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
            if (e->u.member.member && e->u.member.member->offset) {
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
            e = e->u.call.args->items[0];   /* the expected value is advice this compiler does not take */
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

static int cast(AST *e) {
    Type *to = e->type;
    AST *x = e->u.cast.expr;
    Type *from = x->type;

    if (to->kind == TY_VOID) {
        expr(x);
        return -1;
    }
    if (from->kind == TY_ARRAY || from->kind == TY_FUNC || (from->kind == TY_VA_LIST && to->kind == TY_POINTER)) {
        return addr(x);
    }
    if (type_is_complex(to) || type_is_complex(from)) {
        return complex_cast(e);
    }
    if (!lower_check_value_type(e->loc, to) || !lower_check_value_type(x->loc, from)) {
        return iconst(ty(to), 0);
    }
    return convert(expr(x), from, to);
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
    a = lvalue_addr(o);
    old = lvalue_load(o, a);
    if (type_is_floating(t)) {
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
            copy_bytes(a, rv, type_size(t));
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
    if (t->kind == TY_POINTER) {
        size_t size = type_size(t->base);
        lv = lvalue_load(l, a);
        if (sema_const_eval(r, &cv)) {
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

static int is_overflow_builtin(const char *name) {
    return strcmp(name, "__builtin_add_overflow") == 0 || strcmp(name, "__builtin_sub_overflow") == 0
        || strcmp(name, "__builtin_mul_overflow") == 0;
}

/*
 * __builtin_{add,sub,mul}_overflow(a, b, &r): the exact result is computed
 * in 128 bits and stored in r's type; the flag says whether the stored
 * value, widened again, is still the exact result. Two unsigned 64 bit
 * factors can exceed a signed 128 bit product, so they multiply unsigned;
 * every other case is exact in a signed one.
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

    if (args->len != 3 || !type_is_pointer(args->items[2]->type) || !type_is_integer(args->items[0]->type)
            || !type_is_integer(args->items[1]->type) || !type_is_integer(args->items[2]->type->base)) {
        diag_error(e->loc, "'%s' takes two integers and a pointer to an integer", name);
        return iconst(IR_I32, 0);
    }
    a = args->items[0];
    b = args->items[1];
    p = args->items[2];
    ta = a->type;
    tb = b->type;
    tr = type_unqualified(p->type->base);
    w = type_basic(op == IR_MUL && type_is_unsigned(ta) && type_size(ta) >= 8 && type_is_unsigned(tb) && type_size(tb) >= 8
                   ? TY_UINT128 : TY_INT128);
    wa = convert(expr(a), ta, w);
    wb = convert(expr(b), tb, w);
    exact = ir_binop(F, op, IR_I128, wa, wb);
    pv = expr(p);
    narrow = convert(exact, w, tr);
    store_value(tr, pv, narrow);
    back = convert(narrow, tr, w);
    return ir_icmp(F, IR_NE, IR_I128, back, exact);
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
    if (strcmp(name, "__atomic_load_n") == 0) {
        /* a plain load; anything stronger than relaxed gets a full barrier after it */
        long order = 0;
        if (args->len == 2) {
            *result = load_value(e->loc, e->type, expr(args->items[0]));
            sema_const_eval(args->items[1], &order);
            if (order != 0) {
                ir_emit(F, IR_FENCE, IR_VOID, -1, -1, -1);
            }
        }
        return 1;
    }
    if (strcmp(name, "__atomic_store_n") == 0) {
        long order = 0;
        if (args->len == 3) {
            Type *t = type_unqualified(args->items[0]->type->base);
            int a = expr(args->items[0]);
            int v = expr(args->items[1]);
            sema_const_eval(args->items[2], &order);
            if (order != 0) {
                ir_emit(F, IR_FENCE, IR_VOID, -1, -1, -1);
            }
            store_value(t, a, v);
        }
        return 1;
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
    if (strcmp(b, "expect") == 0) {
        if (args->len == 2) {
            *result = expr(args->items[0]);
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

static int call(AST *e) {
    AST *f = e->u.call.func;
    Type *fn = f->type->base;
    ASTList *args = e->u.call.args;
    Symbol *callee = direct_callee(f);
    const char *name = callee ? callee->name : NULL;
    size_t nfixed;
    size_t i;
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
            || strncmp(name, "__sync_", 7) == 0) && builtin(e, name, &name, &result)) {
        return result;
    }

    if (name && codegen_opt_level > 0 && (!callee || !callee->def) && args->len == 1 && (result = math_intrinsic(e, name)) >= 0) {
        return result;
    }

    nfixed = fn->has_proto && fn->variadic ? fn->nparams : args->len;
    if (args->len) {
        vals = xrealloc(NULL, args->len * sizeof(*vals));
        by_addr = xrealloc(NULL, args->len * sizeof(*by_addr));
        types = xrealloc(NULL, args->len * sizeof(*types));
    }
    for (i = 0; i < args->len; i++) {
        AST *a = args->items[i];
        vals[i] = expr(a);
        by_addr[i] = by_address(a->type);
        types[i] = a->type;
    }
    if (!callee) {
        target = expr(f);
    }
    if (by_address(e->type)) {
        /* the result lives in this call's temporary; a record evaluates to its address */
        if (sret) {
            sret_addr = addr_local(e->temp);
        } else {
            result_addr = addr_local(e->temp);
        }
    }
    result = emit_call(name, target, (int)args->len, vals, types, by_addr, nfixed, fn->variadic, e->type, sret_addr, result_addr);
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
        case N_STMT_EXPR: {
            ASTList *items = e->u.stmt.body->u.block.items;
            size_t i;
            int result = -1;
            for (i = 0; i < items->len; i++) {
                AST *item = items->items[i];
                if (i + 1 == items->len && item->kind == N_EXPR_STMT && item->u.stmt.expr) {
                    result = expr(item->u.stmt.expr);
                } else if (item->kind == N_DECLARATION) {
                    local_decl(item);
                } else {
                    stmt(item);
                }
            }
            return result;
        }

        case N_INT:
        case N_CHAR:
        case N_SIZEOF_EXPR:
        case N_SIZEOF_TYPE:
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
            return load_value(e->loc, e->type, addr(e));

        case N_MEMBER:
            if (sema_is_bitfield(e)) {
                return load_bits(e->u.member.member, e->type, addr(e));
            }
            return load_value(e->loc, e->type, addr(e));

        case N_VA_ARG:
            return va_arg_expr(e);

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
                int v = ir_binop(F, op == OP_ADD ? IR_FADD : IR_FSUB, ty(base), load_comp(ls + k * cs, base), load_comp(rs + k * cs, base));
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
                eq[k] = ir_vreg(F, IR_I32);
                i = ir_inst(F, ir_emit(F, IR_FCMP, ty(base), eq[k], load_comp(ls + k * cs, base), load_comp(rs + k * cs, base)));
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

void lower_init_scalar(long off, Type *t, AST *e) {
    int v;
    if (!by_address(t) && !lower_check_value_type(e->loc, t)) {
        return;
    }
    v = expr(e);
    if (by_address(t)) {
        copy_bytes(addr_local(off), v, type_size(t));
    } else {
        store_value(t, addr_local(off), v);
    }
}

void lower_init_string(long off, AST *s, size_t n) {
    copy_bytes(addr_local(off), addr_string(codegen_add_string(s)), n);
}

void lower_init_bitfield(long off, Member *m, AST *item) {
    int a = addr_local(off);
    int v = expr(item);
    store_bits(m, m->type, a, v);
}

static void local_decl(AST *decl) {
    size_t i;
    for (i = 0; i < decl->u.decl.declarators->len; i++) {
        AST *id = decl->u.decl.declarators->items[i];
        Symbol *s = id->sym;
        AST *init = id->u.init_decl.init;
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
            }
            continue;
        }
        if (init->kind == N_INIT_LIST || (s->type->kind == TY_ARRAY && init->kind == N_STRING)) {
            /* members the initializer leaves out are zero */
            zero_bytes(addr_local(s->offset), type_size(s->type));
        }
        codegen_local_initializer(s->type, init, s->offset);
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

        case N_GOTO:
            if (s->sym) {
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

/* can this local live in a register: a scalar whose address is never taken, in a function without setjmp */
static int promotable(const Symbol *s) {
    if (all_in_memory || s->vreg == -1 || s->by_ref || !s->type) {
        return 0;
    }
    if (s->type->quals & TQ_VOLATILE) {
        return 0;
    }
    if (!is_value_type(s->type)) {
        return 0;
    }
    if (s->type->kind == TY_FLOAT16 || (s->type->kind == TY_LDOUBLE && arch_info->ldouble_size > 8 && !arch_info->ldouble_x87)) {
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
            mark_memory(n->u.stmt.expr);
            mark_memory(n->u.stmt.body);
            break;
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
    if (e->type && type_is_complex(e->type) && e->kind != N_CALL && e->kind != N_VA_ARG) {
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
                e->temp = alloc_slot((type_size(e->type) + 7) & ~(size_t)7, 8);
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
            assign_temps(e->u.cast.expr);
            break;
        case N_INIT_LIST:
            for (i = 0; i < e->u.list->len; i++) {
                assign_temps(e->u.list->items[i]);
            }
            break;
        case N_DESIGNATED:
            assign_temps(e->u.desig.init);
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
            for (i = 0; i < s->u.decl.declarators->len; i++) {
                AST *id = s->u.decl.declarators->items[i];
                Symbol *sym = id->sym;
                if (!sym || sym->kind != SYM_VARIABLE || sym->decl != id || !sym->type) {
                    continue;
                }
                if (codegen_sym_is_static_local(sym)) {
                    codegen_static_local(sym);
                } else if (codegen_sym_is_local(sym)) {
                    if (promotable(sym)) {
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
            assign_temps(s->u.stmt.expr);
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
    F->is_global = s->storage != STORAGE_STATIC;
    F->is_variadic = t->variadic;
    F->is_main = strcmp(s->name, "main") == 0;
    F->opt_level = codegen_opt_level;
    frame_off = 0;
    sret_off = 0;
    all_in_memory = 0;
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
            p->offset = alloc_slot(8 * ip->loc.nregs, type_align(p->type) > 8 ? type_align(p->type) : 8);
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
    assign_locals(fdef->u.func.body);

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
        }
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
    F->noopt = all_in_memory;
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
