/*
 * The IR's builder, text dump and verifier (src/ir.h). Lowering appends
 * instructions to the current block; a block ends when a terminator is
 * emitted, and the next instruction (or an explicit ir_place) opens
 * another. Blocks that nothing jumps to are left in place: the verifier
 * and the backends accept them, and the instruction selector can skip
 * them if it likes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "ir.h"
#include "util.h"

#define UNUSED_F(x) (void)(x)

#define GROW(array, count, cap, type) \
    do { \
        if ((count) == (cap)) { \
            (cap) = (cap) ? (cap) * 2 : 16; \
            (array) = xrealloc((array), (size_t)(cap) * sizeof(type)); \
        } \
    } while (0)

IRFunc *ir_func_new(const char *name) {
    IRFunc *f = xrealloc(NULL, sizeof(*f));
    memset(f, 0, sizeof(*f));
    f->name = name;
    f->cur = -1;
    return f;
}

void ir_func_free(IRFunc *f) {
    int i;
    if (!f) {
        return;
    }
    for (i = 0; i < f->nnames; i++) {
        free(f->names[i]);
    }
    free(f->names);
    free(f->blocks);
    free(f->insts);
    free(f->vregs);
    free(f->params);
    free(f->calls);
    free(f->args);
    free(f->switches);
    free(f->jt);
    free(f->argtypes);
    free(f);
}

int ir_vreg(IRFunc *f, IRType type) {
    IRVReg *v;
    GROW(f->vregs, f->nvregs, f->vregs_cap, IRVReg);
    v = &f->vregs[f->nvregs];
    memset(v, 0, sizeof(*v));
    v->type = (unsigned char)type;
    v->reg = -1;
    return f->nvregs++;
}

int ir_vreg_local(IRFunc *f, IRType type, const char *name) {
    int v = ir_vreg(f, type);
    f->vregs[v].is_local = 1;
    f->vregs[v].name = name;
    return v;
}

int ir_block(IRFunc *f) {
    IRBlock *b;
    GROW(f->blocks, f->nblocks, f->blocks_cap, IRBlock);
    b = &f->blocks[f->nblocks];
    b->start = -1;
    b->end = -1;
    return f->nblocks++;
}

static int append(IRFunc *f, IROp op, IRType type, int dst, int a, int b) {
    IRInst *i;
    GROW(f->insts, f->ninsts, f->insts_cap, IRInst);
    i = &f->insts[f->ninsts];
    memset(i, 0, sizeof(*i));
    i->op = (unsigned char)op;
    i->type = (unsigned char)type;
    i->dst = dst;
    i->a = a;
    i->b = b;
    i->aux = -1;
    return f->ninsts++;
}

/* make `block` the current one; it starts at the next instruction */
static void open_block(IRFunc *f, int block) {
    f->blocks[block].start = f->ninsts;
    f->blocks[block].end = f->ninsts;
    f->cur = block;
}

void ir_place(IRFunc *f, int block) {
    if (f->blocks[block].start >= 0) {
        Loc none;
        memset(&none, 0, sizeof(none));
        diag_error(none, "internal error: IR of %s: block %d placed twice", f->name, block);
    }
    if (f->cur >= 0) {
        /* the current block falls through: say so */
        append(f, IR_BR, IR_VOID, -1, -1, -1);
        f->insts[f->ninsts - 1].imm = block;
        f->blocks[f->cur].end = f->ninsts;
    }
    open_block(f, block);
}

int ir_emit(IRFunc *f, IROp op, IRType type, int dst, int a, int b) {
    int index;
    if (f->cur < 0) {
        /* after a terminator: a block nothing jumps to */
        open_block(f, ir_block(f));
    }
    index = append(f, op, type, dst, a, b);
    f->blocks[f->cur].end = f->ninsts;
    if (ir_is_terminator(op)) {
        f->cur = -1;
    }
    return index;
}

IRInst *ir_inst(IRFunc *f, int index) {
    return &f->insts[index];
}

int ir_last(IRFunc *f) {
    return f->ninsts - 1;
}

int ir_add_call(IRFunc *f, const IRCall *c) {
    GROW(f->calls, f->ncalls, f->calls_cap, IRCall);
    f->calls[f->ncalls] = *c;
    return f->ncalls++;
}

int ir_add_arg(IRFunc *f, const IRArg *a) {
    GROW(f->args, f->nargs, f->args_cap, IRArg);
    f->args[f->nargs] = *a;
    return f->nargs++;
}

int ir_add_argtype(IRFunc *f, const ArgType *t) {
    GROW(f->argtypes, f->nargtypes, f->argtypes_cap, ArgType);
    f->argtypes[f->nargtypes] = *t;
    return f->nargtypes++;
}

const char *ir_name(IRFunc *f, const char *name) {
    char *copy = xrealloc(NULL, strlen(name) + 1);
    strcpy(copy, name);
    GROW(f->names, f->nnames, f->names_cap, char *);
    f->names[f->nnames++] = copy;
    return copy;
}

int ir_add_switch(IRFunc *f, const int *targets, int n) {
    int k;
    GROW(f->switches, f->nswitches, f->switches_cap, IRSwitch);
    f->switches[f->nswitches].first = f->njt;
    f->switches[f->nswitches].n = n;
    for (k = 0; k < n; k++) {
        GROW(f->jt, f->njt, f->jt_cap, int);
        f->jt[f->njt++] = targets[k];
    }
    return f->nswitches++;
}

int ir_param_add(IRFunc *f, const IRParam *p) {
    f->params = xrealloc(f->params, (size_t)(f->nparams + 1) * sizeof(*f->params));
    f->params[f->nparams] = *p;
    return f->nparams++;
}

/* the short forms */

/* an i32 immediate is kept sign extended from its low 32 bits, whatever the constant's C type was */
static long fit_imm(IRType type, long imm) {
    return type == IR_I32 ? (long)(int)imm : imm;
}

int ir_iconst(IRFunc *f, IRType type, long value) {
    int dst = ir_vreg(f, type);
    int i = ir_emit(f, IR_ICONST, type, dst, -1, -1);
    f->insts[i].imm = fit_imm(type, value);
    return dst;
}

int ir_binop(IRFunc *f, IROp op, IRType type, int a, int b) {
    int dst = ir_vreg(f, type);
    ir_emit(f, op, type, dst, a, b);
    return dst;
}

int ir_binop_imm(IRFunc *f, IROp op, IRType type, int a, long imm) {
    int dst = ir_vreg(f, type);
    int i = ir_emit(f, op, type, dst, a, -1);
    f->insts[i].flags |= IRF_IMM;
    f->insts[i].imm = fit_imm(type, imm);
    return dst;
}

int ir_unop(IRFunc *f, IROp op, IRType type, int a) {
    int dst = ir_vreg(f, type);
    ir_emit(f, op, type, dst, a, -1);
    return dst;
}

int ir_icmp(IRFunc *f, IRCond cc, IRType type, int a, int b) {
    int dst = ir_vreg(f, IR_I32);
    int i = ir_emit(f, IR_ICMP, type, dst, a, b);
    f->insts[i].cc = (unsigned char)cc;
    return dst;
}

int ir_icmp_imm(IRFunc *f, IRCond cc, IRType type, int a, long imm) {
    int dst = ir_vreg(f, IR_I32);
    int i = ir_emit(f, IR_ICMP, type, dst, a, -1);
    f->insts[i].cc = (unsigned char)cc;
    f->insts[i].flags |= IRF_IMM;
    f->insts[i].imm = fit_imm(type, imm);
    return dst;
}

int ir_load(IRFunc *f, IRType type, int addr, size_t size, int is_signed) {
    int dst = ir_vreg(f, type);
    int i = ir_emit(f, IR_LOAD, type, dst, addr, -1);
    f->insts[i].size = (unsigned char)size;
    if (is_signed) {
        f->insts[i].flags |= IRF_SIGNED;
    }
    return dst;
}

void ir_store(IRFunc *f, IRType type, int addr, int value, size_t size) {
    int i = ir_emit(f, IR_STORE, type, -1, addr, value);
    f->insts[i].size = (unsigned char)size;
}

void ir_br(IRFunc *f, int block) {
    int i = ir_emit(f, IR_BR, IR_VOID, -1, -1, -1);
    f->insts[i].imm = block;
}

void ir_condbr(IRFunc *f, int cond, int then_block, int else_block) {
    int i = ir_emit(f, IR_CONDBR, IR_I32, -1, cond, -1);
    f->insts[i].imm = then_block;
    f->insts[i].aux = else_block;
}

void ir_mov(IRFunc *f, int dst, int src) {
    ir_emit(f, IR_MOV, (IRType)f->vregs[dst].type, dst, src, -1);
}

/* queries */

const char *ir_op_name(IROp op) {
    static const char *const names[] = {
#define X(name, repr) repr,
        IR_OPS(X)
#undef X
    };
    return (unsigned)op < IR_NOPS ? names[op] : "?";
}

const char *ir_type_name(IRType t) {
    switch (t) {
        case IR_VOID: return "void";
        case IR_I32: return "i32";
        case IR_I64: return "i64";
        case IR_I128: return "i128";
        case IR_F32: return "f32";
        case IR_F64: return "f64";
        case IR_F80: return "f80";
    }
    return "?";
}

size_t ir_type_size(IRType t) {
    switch (t) {
        case IR_I32: case IR_F32: return 4;
        case IR_I64: case IR_F64: return 8;
        case IR_I128: case IR_F80: return 16;
        default: return 0;
    }
}

int ir_type_is_float(IRType t) {
    return t == IR_F32 || t == IR_F64 || t == IR_F80;
}

int ir_is_terminator(IROp op) {
    return op == IR_BR || op == IR_CONDBR || op == IR_RET || op == IR_UNREACHABLE || op == IR_SWITCH;
}

int ir_nsuccs(const IRFunc *f, const IRInst *t) {
    switch (t->op) {
        case IR_BR: return 1;
        case IR_CONDBR: return 2;
        case IR_SWITCH: return 1 + f->switches[t->imm].n;
        default: return 0;
    }
}

int ir_succ(const IRFunc *f, const IRInst *t, int k) {
    if (t->op == IR_SWITCH) {
        return k == 0 ? t->aux : f->jt[f->switches[t->imm].first + k - 1];
    }
    return k == 0 ? (int)t->imm : t->aux;
}

void ir_set_succ(IRFunc *f, IRInst *t, int k, int block) {
    if (t->op == IR_SWITCH) {
        if (k == 0) {
            t->aux = block;
        } else {
            f->jt[f->switches[t->imm].first + k - 1] = block;
        }
    } else if (k == 0) {
        t->imm = block;
    } else {
        t->aux = block;
    }
}

/* the registers an instruction reads, up to three (a call's arguments are counted separately) */
static int inst_uses(const IRFunc *f, const IRInst *i, int out[4]) {
    int n = 0;
    if (i->a >= 0) {
        out[n++] = i->a;
    }
    if (i->b >= 0) {
        out[n++] = i->b;
    }
    if (i->op == IR_CALL) {
        const IRCall *c = &f->calls[i->imm];
        if (c->target >= 0) {
            out[n++] = c->target;
        }
        if (c->sret_addr >= 0) {
            out[n++] = c->sret_addr;
        }
    }
    return n;
}

void ir_count_uses(IRFunc *f) {
    int i, k, n;
    int uses[4];
    for (i = 0; i < f->nvregs; i++) {
        f->vregs[i].nuses = 0;
        f->vregs[i].def = -1;
        f->vregs[i].use = -1;
    }
    for (i = 0; i < f->ninsts; i++) {
        const IRInst *ins = &f->insts[i];
        n = inst_uses(f, ins, uses);
        for (k = 0; k < n; k++) {
            f->vregs[uses[k]].nuses++;
            f->vregs[uses[k]].use = i;
        }
        if (ins->op == IR_CALL) {
            const IRCall *c = &f->calls[ins->imm];
            for (k = 0; k < c->nargs; k++) {
                f->vregs[f->args[c->first_arg + k].v].nuses++;
                f->vregs[f->args[c->first_arg + k].v].use = i;
            }
            if (c->result_addr >= 0) {
                f->vregs[c->result_addr].nuses++;
                f->vregs[c->result_addr].use = i;
            }
        }
        if (ins->dst >= 0) {
            f->vregs[ins->dst].def = i;
        }
    }
}

int ir_clobbers_registers(const IRFunc *f, const IRInst *i) {
    UNUSED_F(f);
    switch (i->op) {
        case IR_CALL:
            return 1;
        case IR_SDIV:
        case IR_UDIV:
        case IR_SREM:
        case IR_UREM:
            return i->type == IR_I128;
        case IR_ITOF:
            return i->t2 == IR_I128;
        case IR_FTOI:
            return i->type == IR_I128;
        default:
            return 0;
    }
}

/* the dump */

static const char *cond_name(IRCond cc) {
    switch (cc) {
        case IR_EQ: return "eq";
        case IR_NE: return "ne";
        case IR_LT: return "lt";
        case IR_GE: return "ge";
        case IR_GT: return "gt";
        case IR_LE: return "le";
        case IR_ULT: return "ult";
        case IR_UGE: return "uge";
        case IR_UGT: return "ugt";
        case IR_ULE: return "ule";
    }
    return "?";
}

static void dump_vreg(const IRFunc *f, int v, FILE *out) {
    if (v < 0) {
        fputs("none", out);
    } else if (f->vregs[v].name) {
        fprintf(out, "%%%s.v%d", f->vregs[v].name, v);
    } else {
        fprintf(out, "v%d", v);
    }
}

static void dump_inst(const IRFunc *f, const IRInst *i, FILE *out) {
    int k;
    if (i->dst >= 0) {
        dump_vreg(f, i->dst, out);
        fprintf(out, ":%s = ", ir_type_name((IRType)i->type));
    }
    fputs(ir_op_name((IROp)i->op), out);
    switch (i->op) {
        case IR_ICMP:
        case IR_FCMP:
            fprintf(out, " %s %s", cond_name((IRCond)i->cc), ir_type_name((IRType)i->type));
            break;
        case IR_LOAD:
        case IR_LOADBITS:
            fprintf(out, " %s%u", i->flags & IRF_SIGNED ? "s" : "u", i->size);
            break;
        case IR_STORE:
        case IR_STOREBITS:
        case IR_SEXT:
        case IR_ZEXT:
        case IR_BSWAP:
            fprintf(out, " %u", i->size);
            break;
        case IR_ITOF:
        case IR_FTOI:
            fprintf(out, " %s%s", i->flags & IRF_SIGNED ? "s" : "u", ir_type_name((IRType)i->t2));
            break;
        case IR_FTOF:
        case IR_BITCAST:
            fprintf(out, " %s", ir_type_name((IRType)i->t2));
            break;
        default:
            break;
    }
    if (i->op == IR_BR) {
        fprintf(out, " b%ld", i->imm);
        return;
    }
    if (i->op == IR_CONDBR) {
        fputc(' ', out);
        dump_vreg(f, i->a, out);
        fprintf(out, ", b%ld, b%d", i->imm, i->aux);
        return;
    }
    if (i->op == IR_SWITCH) {
        const IRSwitch *sw = &f->switches[i->imm];
        fputc(' ', out);
        dump_vreg(f, i->a, out);
        fprintf(out, ", default b%d, [", i->aux);
        for (k = 0; k < sw->n; k++) {
            fprintf(out, "%sb%d", k ? " " : "", f->jt[sw->first + k]);
        }
        fputc(']', out);
        return;
    }
    if (i->op == IR_CALL) {
        const IRCall *c = &f->calls[i->imm];
        if (c->name) {
            fprintf(out, " %s(", c->name);
        } else {
            fputc(' ', out);
            dump_vreg(f, c->target, out);
            fputc('(', out);
        }
        for (k = 0; k < c->nargs; k++) {
            const IRArg *a = &f->args[c->first_arg + k];
            if (k) {
                fputs(", ", out);
            }
            if (a->by_addr) {
                fputc('*', out);
            }
            dump_vreg(f, a->v, out);
        }
        fputc(')', out);
        if (c->sret_addr >= 0) {
            fputs(" sret ", out);
            dump_vreg(f, c->sret_addr, out);
        }
        if (c->result_addr >= 0) {
            fputs(" -> *", out);
            dump_vreg(f, c->result_addr, out);
        }
        return;
    }
    if (i->op == IR_ADDR_GLOBAL) {
        fprintf(out, " %s%s", i->sym, i->flags & IRF_EXTERN ? " (extern)" : "");
        return;
    }
    if (i->a >= 0) {
        fputc(' ', out);
        if (i->op == IR_LOAD || i->op == IR_LOADBITS || i->op == IR_STORE || i->op == IR_STOREBITS
                || i->op == IR_COPY || i->op == IR_ZERO || i->op == IR_VASTART || i->op == IR_VAARG) {
            fputc('[', out);
            dump_vreg(f, i->a, out);
            fputc(']', out);
        } else {
            dump_vreg(f, i->a, out);
        }
    }
    if (i->b >= 0) {
        fputs(", ", out);
        if (i->op == IR_COPY) {
            fputc('[', out);
            dump_vreg(f, i->b, out);
            fputc(']', out);
        } else {
            dump_vreg(f, i->b, out);
        }
    }
    switch (i->op) {
        case IR_ICONST:
        case IR_ADDR_LOCAL:
        case IR_ADDR_STRING:
            fprintf(out, " %ld", i->imm);
            break;
        case IR_FCONST:
            fprintf(out, " 0x%lx", (unsigned long)i->imm);
            if (i->type == IR_F80) {
                fprintf(out, " se 0x%x", (unsigned)i->aux);
            }
            break;
        case IR_LOADBITS:
        case IR_STOREBITS:
            fprintf(out, " bits %ld+%ld", i->imm & 0xff, i->imm >> 8);
            break;
        case IR_RET:
            if (i->flags & IRF_RECORD) {
                fputs(" (record)", out);
            }
            break;
        case IR_VAARG:
            fprintf(out, " temp %ld", i->imm);
            break;
        case IR_FRINT:
            fprintf(out, " %s", i->imm == 0 ? "floor" : i->imm == 1 ? "ceil" : i->imm == 2 ? "trunc" : "round");
            break;
        case IR_COPY:
        case IR_ZERO:
            fprintf(out, " %ld", i->imm);
            break;
        default:
            if (i->flags & IRF_IMM) {
                fprintf(out, "%s%ld", i->a >= 0 ? ", " : " ", i->imm);
            }
            break;
    }
}

void ir_dump(const IRFunc *f, FILE *out) {
    int b, i, k;
    fprintf(out, "%s%s(", f->is_global ? "" : "static ", f->name);
    for (k = 0; k < f->nparams; k++) {
        const IRParam *p = &f->params[k];
        if (k) {
            fputs(", ", out);
        }
        if (p->vreg >= 0) {
            dump_vreg(f, p->vreg, out);
            fprintf(out, ":%s", ir_type_name((IRType)f->vregs[p->vreg].type));
        } else {
            fprintf(out, "[fp%+ld]", p->offset);
        }
    }
    fprintf(out, ")%s:  locals %ld bytes\n", f->is_variadic ? " ..." : "", f->locals_size);
    for (b = 0; b < f->nblocks; b++) {
        const IRBlock *blk = &f->blocks[b];
        fprintf(out, "b%d:", b);
        if (blk->start < 0) {
            fputs("  (never placed)\n", out);
            continue;
        }
        if (blk->start == blk->end) {
            fputc('\n', out);
        }
        for (i = blk->start; i < blk->end; i++) {
            fputs(i == blk->start ? "  " : "     ", out);
            dump_inst(f, &f->insts[i], out);
            fputc('\n', out);
        }
    }
    fputc('\n', out);
}

/*
 * The verifier. Structure first: every placed block ends in a terminator
 * and jumps to placed blocks, registers and tables are in range. Then
 * definitions: every read of a temporary (not a C variable, which may be
 * read before it is written) is preceded by a write on every path from
 * the entry, computed as a forward dataflow over bit sets.
 */
static int problems = 0;
static const IRFunc *vf = NULL;

static void problem(const char *what, int inst) {
    Loc none;
    memset(&none, 0, sizeof(none));
    if (inst >= 0) {
        diag_error(none, "internal error: IR of %s, instruction %d (%s): %s", vf->name, inst, ir_op_name((IROp)vf->insts[inst].op), what);
    } else {
        diag_error(none, "internal error: IR of %s: %s", vf->name, what);
    }
    problems++;
}

static int block_ok(int b) {
    return b >= 0 && b < vf->nblocks && vf->blocks[b].start >= 0;
}

static int vreg_ok(int v) {
    return v >= 0 && v < vf->nvregs;
}

typedef unsigned long BitWord;
#define BITWORD_BITS (8 * sizeof(BitWord))

static void bit_set(BitWord *s, int bit) {
    s[bit / BITWORD_BITS] |= 1ul << (bit % BITWORD_BITS);
}

static int has_bit(const BitWord *s, int bit) {
    return (s[bit / BITWORD_BITS] >> (bit % BITWORD_BITS)) & 1;
}

int ir_verify(IRFunc *f) {
    int b, i, k, n, changed, iterations;
    size_t words;
    BitWord *in = NULL;       /* per block: the registers defined on entry */
    BitWord *out = NULL;      /* and on exit */
    BitWord *cur = NULL;
    int *npreds = NULL;
    int uses[4];

    vf = f;
    problems = 0;
    for (b = 0; b < f->nblocks; b++) {
        const IRBlock *blk = &f->blocks[b];
        if (blk->start < 0) {
            continue;
        }
        if (blk->end <= blk->start || !ir_is_terminator((IROp)f->insts[blk->end - 1].op)) {
            problem("block does not end in a terminator", blk->end - 1);
        }
        for (i = blk->start; i < blk->end; i++) {
            const IRInst *in_ = &f->insts[i];
            if (i != blk->end - 1 && ir_is_terminator((IROp)in_->op)) {
                problem("terminator in the middle of a block", i);
            }
            if (in_->dst != -1 && !vreg_ok(in_->dst)) {
                problem("destination register out of range", i);
            }
            if (in_->a != -1 && !vreg_ok(in_->a)) {
                problem("operand register out of range", i);
            }
            if (in_->b != -1 && !vreg_ok(in_->b)) {
                problem("operand register out of range", i);
            }
            if (in_->op == IR_BR && !block_ok((int)in_->imm)) {
                problem("branch to a block that does not exist", i);
            }
            if (in_->op == IR_CONDBR && (!block_ok((int)in_->imm) || !block_ok(in_->aux))) {
                problem("conditional branch to a block that does not exist", i);
            }
            if (in_->op == IR_SWITCH) {
                if (in_->imm < 0 || in_->imm >= f->nswitches) {
                    problem("switch table index out of range", i);
                } else {
                    for (k = 0; k < ir_nsuccs(f, in_); k++) {
                        if (!block_ok(ir_succ(f, in_, k))) {
                            problem("switch to a block that does not exist", i);
                        }
                    }
                }
            }
            if (in_->op == IR_CALL && (in_->imm < 0 || in_->imm >= f->ncalls)) {
                problem("call table index out of range", i);
            }
            if (in_->op == IR_CALL) {
                const IRCall *c = &f->calls[in_->imm];
                for (k = 0; k < c->nargs; k++) {
                    if (!vreg_ok(f->args[c->first_arg + k].v)) {
                        problem("call argument register out of range", i);
                    }
                }
            }
            if (in_->dst >= 0 && vreg_ok(in_->dst) && in_->op != IR_STOREBITS && in_->op != IR_CALL
                    && in_->op != IR_ICMP && in_->op != IR_FCMP && in_->type != f->vregs[in_->dst].type) {
                problem("destination register has another type", i);
            }
            if ((in_->op == IR_ICMP || in_->op == IR_FCMP) && vreg_ok(in_->dst) && f->vregs[in_->dst].type != IR_I32) {
                problem("comparison result is not an i32", i);
            }
        }
    }
    if (problems) {
        return problems;
    }

    /* definitions: forward dataflow over the blocks */
    words = ((size_t)f->nvregs + BITWORD_BITS - 1) / BITWORD_BITS;
    if (words == 0) {
        words = 1;
    }
    in = xrealloc(NULL, (size_t)f->nblocks * words * sizeof(BitWord));
    out = xrealloc(NULL, (size_t)f->nblocks * words * sizeof(BitWord));
    cur = xrealloc(NULL, words * sizeof(BitWord));
    npreds = xrealloc(NULL, (size_t)f->nblocks * sizeof(int));
    memset(npreds, 0, (size_t)f->nblocks * sizeof(int));
    for (b = 0; b < f->nblocks; b++) {
        const IRInst *t;
        if (f->blocks[b].start < 0) {
            continue;
        }
        t = &f->insts[f->blocks[b].end - 1];
        for (k = 0; k < ir_nsuccs(f, t); k++) {
            npreds[ir_succ(f, t, k)]++;
        }
    }
    /* start optimistic (everything defined) except at the entry, where only the parameters are */
    memset(in, 0xff, (size_t)f->nblocks * words * sizeof(BitWord));
    memset(out, 0xff, (size_t)f->nblocks * words * sizeof(BitWord));
    memset(in, 0, words * sizeof(BitWord));
    for (k = 0; k < f->nparams; k++) {
        if (f->params[k].vreg >= 0) {
            bit_set(in, f->params[k].vreg);
        }
    }
    for (b = 0; b < f->nblocks; b++) {
        if (f->blocks[b].start >= 0 && npreds[b] == 0 && b != 0) {
            memset(in + (size_t)b * words, 0xff, words * sizeof(BitWord));     /* unreachable: anything goes */
        }
    }
    iterations = 0;
    do {
        changed = 0;
        for (b = 0; b < f->nblocks; b++) {
            const IRBlock *blk = &f->blocks[b];
            const IRInst *t;
            size_t w;
            if (blk->start < 0) {
                continue;
            }
            memcpy(cur, in + (size_t)b * words, words * sizeof(BitWord));
            for (i = blk->start; i < blk->end; i++) {
                const IRInst *ins = &f->insts[i];
                if (ins->dst >= 0) {
                    bit_set(cur, ins->dst);
                }
            }
            if (memcmp(cur, out + (size_t)b * words, words * sizeof(BitWord)) != 0) {
                memcpy(out + (size_t)b * words, cur, words * sizeof(BitWord));
                changed = 1;
            }
            /* successors: in = intersection of the predecessors' out */
            t = &f->insts[blk->end - 1];
            for (k = 0; k < ir_nsuccs(f, t); k++) {
                int s = ir_succ(f, t, k);
                for (w = 0; w < words; w++) {
                    BitWord nw = in[(size_t)s * words + w] & out[(size_t)b * words + w];
                    if (nw != in[(size_t)s * words + w]) {
                        in[(size_t)s * words + w] = nw;
                        changed = 1;
                    }
                }
            }
        }
        iterations++;
    } while (changed && iterations < 1000);

    for (b = 0; b < f->nblocks; b++) {
        const IRBlock *blk = &f->blocks[b];
        if (blk->start < 0) {
            continue;
        }
        memcpy(cur, in + (size_t)b * words, words * sizeof(BitWord));
        for (i = blk->start; i < blk->end; i++) {
            const IRInst *ins = &f->insts[i];
            n = inst_uses(f, ins, uses);
            if (ins->op == IR_CALL) {
                const IRCall *c = &f->calls[ins->imm];
                for (k = 0; k < c->nargs; k++) {
                    int v = f->args[c->first_arg + k].v;
                    if (!f->vregs[v].is_local && !has_bit(cur, v)) {
                        problem("call argument read before it is written", i);
                    }
                }
                if (c->result_addr >= 0 && !f->vregs[c->result_addr].is_local && !has_bit(cur, c->result_addr)) {
                    problem("result address read before it is written", i);
                }
            }
            for (k = 0; k < n; k++) {
                if (!f->vregs[uses[k]].is_local && !has_bit(cur, uses[k])) {
                    problem("register read before it is written on some path", i);
                }
            }
            if (ins->dst >= 0) {
                bit_set(cur, ins->dst);
            }
        }
    }
    free(in);
    free(out);
    free(cur);
    free(npreds);
    return problems;
}
