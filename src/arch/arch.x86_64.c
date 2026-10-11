#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arch.h"
#include "ir.h"
#include "peep.h"
#include "regalloc.h"
#include "util.h"

/*
 * x86_64 backend, emitting AT&T syntax assembly for the System V ABI as
 * used by Linux (ELF). The C runtime supplies _start and calls main.
 *
 * Instruction selection over the IR (src/ir.h). The allocator
 * (src/regalloc.c) puts a virtual register in a machine register (rdi,
 * rsi, r8, r9 and xmm2-xmm7 for values no call sequence overlaps, r11
 * and xmm8-xmm15 for values not live across a call, the callee-saved
 * rbx and r12-r15 for the rest) or in a frame slot below rbp. An
 * instruction reads a slot operand into a scratch register (rax, rcx,
 * rdx; xmm0, xmm1), computes, and stores a slot result; rsi and rdi
 * also serve the block copies and r10 the record pieces and the target
 * of an indirect call. An __int128 is a pair rax:rdx (and rcx:rsi for a
 * second operand); an x87 long double lives in its 16 byte slot and
 * visits the x87 stack for each operation, which is otherwise empty.
 * rbp is the frame pointer; nothing is ever pushed, so rsp stays 16
 * byte aligned.
 *
 * The statics the two backends define under the same name (the arch.h
 * implementation and the helpers both need) carry the x64_ prefix here
 * and a64_ in src/arch/arch.arm64.c, so that both can live in one
 * translation unit, the amalgamation (tools/amalgamate.sh);
 * tests/statics.sh checks.
 */

/* System V x86_64: LP64, 80-bit long double in 16 bytes, va_list is a 24 byte struct array */
static const ArchInfo x64_info = {
    "x86_64",
    1,          /* char is signed */
    2, 4, 8, 8, /* short, int, long, long long */
    16, 16,     /* __int128 */
    8,          /* pointers */
    4, 8,       /* float, double */
    16, 16,     /* long double */
    24, 8,      /* va_list */
    1,          /* __va_list_tag[1] */
    1,          /* long double is the x87 80 bit format */
    0           /* roundsd is SSE4.1, past the baseline */
};

static FILE *x64_cg_out = NULL;

static int x64_nlabels = 0;         /* backend-private labels, .Lcg<n>, and the block labels .L<n> */
static long va_save_off = 0;    /* variadic function: the register save area, from rbp */

/* the general registers this backend touches, by number; the argument registers are the first six */
enum { RDI, RSI, RDX, RCX, R8, R9, RAX, R10, R11, RBX, R12, R13, R14, R15, NREGS };
static const char *const reg64[] = { "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9", "%rax", "%r10", "%r11", "%rbx", "%r12", "%r13", "%r14", "%r15" };
static const char *const reg32[] = { "%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d", "%eax", "%r10d", "%r11d", "%ebx", "%r12d", "%r13d", "%r14d", "%r15d" };
static const char *const reg16[] = { "%di", "%si", "%dx", "%cx", "%r8w", "%r9w", "%ax", "%r10w", "%r11w", "%bx", "%r12w", "%r13w", "%r14w", "%r15w" };
static const char *const reg8[] = { "%dil", "%sil", "%dl", "%cl", "%r8b", "%r9b", "%al", "%r10b", "%r11b", "%bl", "%r12b", "%r13b", "%r14b", "%r15b" };

static int x64_peep_rules(void);

static void x64_emit(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    line[0] = '\t';
    vsnprintf(line + 1, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    peep_add(line);
}

/* ------------------------------------------------------------------ */
/* peephole rules                                                     */
/* ------------------------------------------------------------------ */

/* "movq %REG, off(%rbp)" or "movsd %xmmN, off(%rbp)": a store of a register to a frame slot */
static int parse_slot_store(const char *l, char *reg, long *off) {
    char extra;
    if (sscanf(l, "\tmovq %7[%a-z0-9], %ld(%%rbp%c", reg, off, &extra) == 3 && extra == ')' && reg[1] == 'r') {
        return 1;
    }
    return sscanf(l, "\tmovsd %7[%a-z0-9], %ld(%%rbp%c", reg, off, &extra) == 3 && extra == ')' && reg[1] == 'x';
}

/* "movq off(%rbp), %REG" or "movsd off(%rbp), %xmmN": a load of a frame slot */
static int parse_slot_load(const char *l, char *reg, long *off) {
    size_t n = strlen(l);
    if (sscanf(l, "\tmovq %ld(%%rbp), %7s", off, reg) == 2 && reg[1] == 'r' && l[n - 1] != ')') {
        return 1;
    }
    return sscanf(l, "\tmovsd %ld(%%rbp), %7s", off, reg) == 2 && reg[1] == 'x' && l[n - 1] != ')';
}

static int x64_peep_rules(void) {
    int n = peep_n;
    const char *with[2];
    char buf[256];
    char sreg[8], reg[8];
    long soff, off;

    /* a store of a register to a frame slot, then a load of the slot: the value is still in the register */
    if (n >= 2 && parse_slot_store(peep_lines[n - 2], sreg, &soff) && parse_slot_load(peep_lines[n - 1], reg, &off)
            && off == soff && sreg[1] == reg[1]) {
        if (strcmp(sreg, reg) == 0) {
            peep_replace(n - 1, 1, with, 0);
        } else {
            snprintf(buf, sizeof(buf), "\t%s %s, %s", reg[1] == 'x' ? "movaps" : "movq", sreg, reg);
            with[0] = buf;
            peep_replace(n - 1, 1, with, 1);
        }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static int x64_log2_of(size_t n) {
    int b = 0;
    while (n > 1) {
        n >>= 1;
        b++;
    }
    return b;
}

static int fits_i32(long v) {
    return v >= -2147483648L && v <= 2147483647L;
}

/* the name of register r for a `size` byte access, or for an IR type */
static const char *rsz(int r, size_t size) {
    switch (size) {
        case 1: return reg8[r];
        case 2: return reg16[r];
        case 4: return reg32[r];
        default: return reg64[r];
    }
}

static const char *rt(int r, IRType t) {
    return t == IR_I32 ? reg32[r] : reg64[r];
}

/* the operation suffix for a type */
static char sfx(IRType t) {
    return t == IR_I32 ? 'l' : 'q';
}

static const char *fsuf(IRType t) {
    return t == IR_F32 ? "ss" : "sd";
}

static const char *xmm(int n) {
    static char bufs[8][8];
    static int next = 0;
    char *b = bufs[next++ & 7];
    snprintf(b, 8, "%%xmm%d", n);
    return b;
}

static void x64_cg_begin(FILE *out) {
    x64_cg_out = out;
    peep_begin(out, x64_peep_rules, getenv("KCC_NOPEEP") == NULL);
}

static void x64_cg_end(void) {
    x64_emit(".section .note.GNU-stack,\"\",@progbits");
}

/* ------------------------------------------------------------------ */
/* data                                                               */
/* ------------------------------------------------------------------ */

static void x64_cg_data_begin(const char *name, int is_global, size_t align, int zero) {
    x64_emit(zero ? ".bss" : ".data");
    if (is_global) {
        x64_emit(".globl %s", name);
    }
    if (align > 1) {
        x64_emit(".p2align %d", x64_log2_of(align));
    }
    x64_emit(".type %s, @object", name);
    peep_flush();
    fprintf(x64_cg_out, "%s:\n", name);
}

static void x64_cg_data_int(size_t size, long value) {
    switch (size) {
        case 1: x64_emit(".byte %ld", value & 0xff); break;
        case 2: x64_emit(".short %ld", value & 0xffff); break;
        case 4: x64_emit(".long %ld", value & 0xffffffff); break;
        case 16: x64_emit(".quad %ld", value); x64_emit(".quad %d", value < 0 ? -1 : 0); break;
        default: x64_emit(".quad %ld", value); break;
    }
}

static void x64_cg_data_addr(const char *name, long addend) {
    x64_emit(".quad %s%+lld", name, addend);
}

static void x64_cg_data_string_addr(int label, long addend) {
    x64_emit(".quad .L.str.%d%+lld", label, addend);
}

static void x64_cg_data_bytes(const char *bytes, size_t len) {
    size_t i;
    peep_flush();
    fputs("\t.ascii \"", x64_cg_out);
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (c == '"' || c == '\\') {
            fprintf(x64_cg_out, "\\%c", c);
        } else if (c >= 32 && c < 127) {
            fputc(c, x64_cg_out);
        } else {
            fprintf(x64_cg_out, "\\%03o", c);
        }
    }
    fputs("\"\n", x64_cg_out);
}

static void x64_cg_data_zero(size_t size) {
    if (size) {
        x64_emit(".zero %lu", (unsigned long)size);
    }
}

static void x64_cg_string(int label, const char *bytes, size_t len) {
    x64_emit(".section .rodata");
    peep_flush();
    fprintf(x64_cg_out, ".L.str.%d:\n", label);
    x64_cg_data_bytes(bytes, len);
}

/* the return address is at 8(%rbp), the caller's outgoing area above it */
static long x64_cg_stack_param_offset(size_t offset) {
    return 16 + (long)offset;
}

/* ------------------------------------------------------------------ */
/* the calling convention                                             */
/* ------------------------------------------------------------------ */

/*
 * System V: six integer argument registers, variadic or not. A record of
 * up to 16 bytes with no floating members is one or two INTEGER
 * eightbytes, passed in registers when enough are left and otherwise
 * on the stack; a larger record is class MEMORY and is copied onto the
 * stack. Records of up to 16 bytes are returned in rax and rdx, larger
 * ones through a hidden pointer passed in rdi and returned in rax.
 */
static void x64_cg_args_begin(ArgAlloc *a, size_t nfixed, int sret) {
    a->nregs = sret ? 1 : 0;
    a->nfregs = 0;
    a->nstack = 0;
    a->nfixed = nfixed;
    a->index = 0;
}

/* every stack argument takes whole eightbytes */
static void x64_stack_bytes(ArgAlloc *a, size_t n, size_t align, ArgLoc *loc) {
    if (align > 8) {
        a->nstack = (a->nstack + 15) & ~(size_t)15;
    }
    loc->in_regs = 0;
    loc->offset = a->nstack;
    loc->size = 8 * n;
    a->nstack += 8 * n;
}

/* how many of a record's eightbytes are class SSE */
static size_t sse_count(const ArgType *t, size_t n) {
    size_t k;
    size_t nf = 0;
    for (k = 0; k < n; k++) {
        nf += (t->sse >> k) & 1;
    }
    return nf;
}

static void x64_cg_args_next(ArgAlloc *a, const ArgType *t, ArgLoc *loc) {
    ArgKind kind = t->kind;
    size_t size = t->size;
    int is_record = kind == ARG_RECORD || kind == ARG_RECORD_MEMORY;
    size_t n = is_record || size == 16 ? (size + 7) / 8 : 1;
    size_t nf = is_record ? sse_count(t, n) : 0;

    a->index++;
    loc->by_ref = 0;
    loc->in_fregs = 0;
    loc->reg = loc->freg = loc->nregs = loc->offset = loc->size = loc->copy = loc->copy_size = 0;
    loc->type = *t;
    loc->type.hfa = 0;      /* HFAs are arm64's; here the eightbyte classes decide */
    if (kind == ARG_CX87) {
        x64_stack_bytes(a, 4, 16, loc);     /* class COMPLEX_X87: in memory */
        return;
    }
    if (kind == ARG_FLOAT) {
        if (size == 16) {
            x64_stack_bytes(a, 2, 16, loc);     /* class X87: always in memory */
            return;
        }
        if (a->nfregs < 8) {
            loc->in_regs = 1;
            loc->in_fregs = 1;
            loc->reg = loc->freg = a->nfregs++;
            loc->nregs = 1;
            return;
        }
        x64_stack_bytes(a, 1, 8, loc);
        return;
    }
    /* each eightbyte needs a register of its class, or the whole argument goes on the stack */
    if (kind != ARG_RECORD_MEMORY && size <= 16 && a->nregs + n - nf <= 6 && a->nfregs + nf <= 8) {
        loc->in_regs = 1;
        loc->reg = a->nregs;
        loc->freg = a->nfregs;
        loc->nregs = n;
        a->nregs += n - nf;
        a->nfregs += nf;
        return;
    }
    x64_stack_bytes(a, n, t->align, loc);
}

static size_t x64_cg_args_end(ArgAlloc *a, ArgLoc *locs, size_t n) {
    UNUSED(locs);
    UNUSED(n);
    return a->nstack;
}

static int x64_cg_record_ret_regs(const ArgType *t) {
    return t->kind == ARG_CX87 || (t->size <= 16 && t->kind != ARG_RECORD_MEMORY);
}

/* ------------------------------------------------------------------ */
/* instruction selection                                              */
/* ------------------------------------------------------------------ */

static IRFunc *x64_F = NULL;
static int x64_label_base = 0;      /* block b's label is .L<x64_label_base + b> */
static int *x64_fold_of = NULL;     /* per instruction: the address computation folded into this memory access, or -1 */
static int *x64_folded = NULL;      /* per instruction: 1 when it was folded into its user and is not emitted */
static int *x64_npreds = NULL;      /* per block */

/* the allocatable registers in the allocator's three tiers: arguments, other caller-saved, callee-saved (no vector register is) */
static const int x64_iregs[] = { RDI, RSI, R8, R9, R11, RBX, R12, R13, R14, R15 };
static const int x64_fregs[] = { 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

static int x64_cg_clobbers(const IRFunc *f, const IRInst *i);

static const RegTarget x64_regtarget = {
    { x64_iregs, 4, 5, 10, -1 },
    { x64_fregs, 6, 14, 14, -1 },
    x64_cg_clobbers
};

/* what an instruction's code writes besides its destination: the runtime calls everything caller-saved; rsi and rdi serve the __int128 pairs, the block copies, bit-field stores, va_arg and a record return */
static int x64_cg_clobbers(const IRFunc *f, const IRInst *i) {
    if (ir_clobbers_registers(f, i)) {
        return RA_CLOBBER_CALL;
    }
    if (regalloc_has_wide(f, i) || i->op == IR_COPY || i->op == IR_ZERO || i->op == IR_STOREBITS || i->op == IR_VAARG
            || (i->op == IR_RET && (i->flags & IRF_RECORD))) {
        return RA_CLOBBER_ARGS;
    }
    return RA_CLOBBER_NONE;
}

static IRType x64_vtype(int v) {
    return (IRType)x64_F->vregs[v].type;
}

/* the machine register holding `v`, or -1 when it has a frame slot */
static int x64_loc_reg(int v) {
    const IRVReg *r = &x64_F->vregs[v];
    if (r->reg < 0) {
        return -1;
    }
    return ir_type_is_float((IRType)r->type) ? x64_fregs[r->reg] : x64_iregs[r->reg];
}

/* the slot operand of a virtual register */
static const char *slot(int v) {
    static char bufs[4][32];
    static int next = 0;
    char *b = bufs[next++ & 3];
    snprintf(b, 32, "%ld(%%rbp)", x64_F->vregs[v].offset);
    return b;
}

static const char *slot_at(int v, long add) {
    static char bufs[4][32];
    static int next = 0;
    char *b = bufs[next++ & 3];
    snprintf(b, 32, "%ld(%%rbp)", x64_F->vregs[v].offset + add);
    return b;
}

/* register `scratch` (or its own) = v */
static int use_r(int v, int scratch) {
    int r = x64_loc_reg(v);
    if (r >= 0) {
        return r;
    }
    if (x64_F->vregs[v].offset) {
        x64_emit("movq %s, %s", slot(v), reg64[scratch]);
    }
    return scratch;
}

static int x64_use_x(int v, int scratch) {
    int r = x64_loc_reg(v);
    if (r >= 0) {
        return r;
    }
    if (x64_F->vregs[v].offset) {
        x64_emit("movsd %s, %s", slot(v), xmm(scratch));
    }
    return scratch;
}

/* the register to compute `v` into */
static int x64_def_reg(int v, int scratch) {
    int r = x64_loc_reg(v);
    return r >= 0 ? r : scratch;
}

/* `v` has been computed into register r: move it to its register, or store it to its slot */
static void def_done_r(int v, int r) {
    int p = x64_loc_reg(v);
    if (p >= 0) {
        if (p != r) {
            x64_emit("movq %s, %s", reg64[r], reg64[p]);
        }
    } else if (x64_F->vregs[v].offset) {
        x64_emit("movq %s, %s", reg64[r], slot(v));
    }
}

static void x64_def_done_x(int v, int r) {
    int p = x64_loc_reg(v);
    if (p >= 0) {
        if (p != r) {
            x64_emit("movaps %s, %s", xmm(r), xmm(p));
        }
    } else if (x64_F->vregs[v].offset) {
        x64_emit("movsd %s, %s", xmm(r), slot(v));
    }
}

/* an __int128 lives in a 16 byte slot: lo:hi = v, and back */
static void x64_use_pair(int v, int lo, int hi) {
    if (!x64_F->vregs[v].offset) {
        return;
    }
    x64_emit("movq %s, %s", slot(v), reg64[lo]);
    x64_emit("movq %s, %s", slot_at(v, 8), reg64[hi]);
}

static void x64_def_pair_done(int v, int lo, int hi) {
    if (!x64_F->vregs[v].offset) {
        return;
    }
    x64_emit("movq %s, %s", reg64[lo], slot(v));
    x64_emit("movq %s, %s", reg64[hi], slot_at(v, 8));
}

/* an x87 long double: push it from its slot, and pop st(0) into a slot */
static void use_x87(int v) {
    if (x64_F->vregs[v].offset) {
        x64_emit("fldt %s", slot(v));
    } else {
        x64_emit("fldz");
    }
}

static void def_x87_done(int v) {
    if (x64_F->vregs[v].offset) {
        x64_emit("fstpt %s", slot(v));
    } else {
        x64_emit("fstp %%st(0)");
    }
}

static const char *x64_blabel(int b) {
    static char bufs[4][16];
    static int next = 0;
    char *buf = bufs[next++ & 3];
    snprintf(buf, 16, ".L%d", x64_label_base + b);
    return buf;
}

static const char *icc(IRCond cc) {
    switch (cc) {
        case IR_EQ: return "e";
        case IR_NE: return "ne";
        case IR_LT: return "l";
        case IR_GE: return "ge";
        case IR_GT: return "g";
        case IR_LE: return "le";
        case IR_ULT: return "b";
        case IR_UGE: return "ae";
        case IR_UGT: return "a";
        case IR_ULE: return "be";
    }
    return "ne";
}

static const char *invert_cc(const char *c) {
    static const char *const pairs[] = { "e", "ne", "l", "ge", "g", "le", "b", "ae", "a", "be", NULL };
    int i;
    for (i = 0; pairs[i]; i++) {
        if (strcmp(pairs[i], c) == 0) {
            return pairs[i ^ 1];
        }
    }
    return NULL;
}

/* the memory access size of a load, store or bit-field instruction */
static int x64_access_size(const IRInst *i) {
    if (i->op == IR_LOAD || i->op == IR_STORE || i->op == IR_LOADBITS || i->op == IR_STOREBITS) {
        return i->size ? i->size : (int)ir_type_size((IRType)i->type);
    }
    return 8;
}

/*
 * Addressing modes. An addr_local whose only use is the address of a
 * load or store, or an add of a constant to a pointer whose only use is
 * the access right after it, is folded into that access as off(%rbp) or
 * off(%reg); the folded temporary gets no place (this runs before the
 * allocator).
 */
static void x64_find_folds(void) {
    int i;
    for (i = 0; i < x64_F->ninsts; i++) {
        x64_fold_of[i] = -1;
        x64_folded[i] = 0;
    }
    for (i = 0; i < x64_F->ninsts; i++) {
        const IRInst *d = &x64_F->insts[i];
        const IRVReg *v;
        const IRInst *u;
        if (d->dst < 0 || (d->op != IR_ADDR_LOCAL && !(d->op == IR_ADD && (d->flags & IRF_IMM) && d->type == IR_I64))) {
            continue;
        }
        v = &x64_F->vregs[d->dst];
        if (v->is_local || v->nuses != 1 || v->def != i || v->use <= i) {
            continue;
        }
        u = &x64_F->insts[v->use];
        if (u->op != IR_LOAD && u->op != IR_STORE && u->op != IR_LOADBITS && u->op != IR_STOREBITS) {
            continue;
        }
        if (u->a != d->dst || u->b == d->dst || !fits_i32(d->imm)) {
            continue;
        }
        if (d->op == IR_ADD && v->use != i + 1) {
            continue;
        }
        UNUSED(x64_access_size);
        x64_fold_of[v->use] = i;
        x64_folded[i] = 1;
        x64_F->vregs[d->dst].folded = 1;
    }
}

/* the address operand of memory instruction `i`: "off(%rbp)", "off(%reg)" or "(%reg)"; the base is loaded into `scratch` if needed */
static const char *x64_mem_operand(int i, int scratch, long add, char *buf, size_t n) {
    const IRInst *ins = &x64_F->insts[i];
    if (x64_fold_of[i] >= 0) {
        const IRInst *d = &x64_F->insts[x64_fold_of[i]];
        if (d->op == IR_ADDR_LOCAL) {
            snprintf(buf, n, "%ld(%%rbp)", d->imm + add);
        } else {
            int base = use_r(d->a, scratch);
            if (d->imm + add) {
                snprintf(buf, n, "%ld(%s)", d->imm + add, reg64[base]);
            } else {
                snprintf(buf, n, "(%s)", reg64[base]);
            }
        }
        return buf;
    }
    if (add) {
        snprintf(buf, n, "%ld(%s)", add, reg64[use_r(ins->a, scratch)]);
    } else {
        snprintf(buf, n, "(%s)", reg64[use_r(ins->a, scratch)]);
    }
    return buf;
}

/* `dst` (a 64 bit register) = the `len` (1..8) bytes at off(base), zero extended; r10 is scratch */
static void x64_load_word(int dst, int base, size_t off, size_t len) {
    size_t pos = 0;
    while (pos < len) {
        size_t chunk = len - pos >= 8 ? 8 : len - pos >= 4 ? 4 : len - pos >= 2 ? 2 : 1;
        int d = pos == 0 ? dst : R10;
        switch (chunk) {
            case 8: x64_emit("movq %lu(%s), %s", (unsigned long)(off + pos), reg64[base], reg64[d]); break;
            case 4: x64_emit("movl %lu(%s), %s", (unsigned long)(off + pos), reg64[base], reg32[d]); break;
            case 2: x64_emit("movzwl %lu(%s), %s", (unsigned long)(off + pos), reg64[base], reg32[d]); break;
            default: x64_emit("movzbl %lu(%s), %s", (unsigned long)(off + pos), reg64[base], reg32[d]); break;
        }
        if (pos) {
            x64_emit("shlq $%lu, %%r10", (unsigned long)(8 * pos));
            x64_emit("orq %%r10, %s", reg64[dst]);
        }
        pos += chunk;
    }
}

/* copy `size` bytes from (%rsi) to (%rdi), both set up by the caller */
static void rep_copy(size_t size) {
    x64_emit("movq $%lu, %%rcx", (unsigned long)size);
    x64_emit("rep movsb");
}

/* save (or restore) the callee-saved registers the allocator used */
static void x64_saved_regs(int restore) {
    long off = x64_F->saved_off;
    int k;
    for (k = 0; k < x64_F->nsaved_ints; k++) {
        if (restore) {
            x64_emit("movq %ld(%%rbp), %s", off, reg64[x64_iregs[x64_F->saved_ints[k]]]);
        } else {
            x64_emit("movq %s, %ld(%%rbp)", reg64[x64_iregs[x64_F->saved_ints[k]]], off);
        }
        off += 8;
    }
}

/* the epilogue, at every return */
static void x64_epilogue(void) {
    x64_saved_regs(1);
    x64_emit("leave");
    x64_emit("ret");
}

/* dst = a; the mov is skipped when they are the same register */
static void move(int dst, int src, IRType t) {
    if (dst != src) {
        x64_emit("mov%c %s, %s", sfx(t), rt(src, t), rt(dst, t));
    }
}

/* the bit-field unit at `m`, zero extended into rax */
static void load_unit(const char *m, size_t size, int dst) {
    switch (size) {
        case 1: x64_emit("movzbq %s, %s", m, reg64[dst]); break;
        case 2: x64_emit("movzwq %s, %s", m, reg64[dst]); break;
        case 4: x64_emit("movl %s, %s", m, reg32[dst]); break;
        default: x64_emit("movq %s, %s", m, reg64[dst]); break;
    }
}

static void extract_bits(int r, unsigned bit_offset, unsigned bits, int is_signed) {
    if (64 - bit_offset - bits) {
        x64_emit("shlq $%u, %s", 64 - bit_offset - bits, reg64[r]);
    }
    if (64 - bits) {
        x64_emit("%s $%u, %s", is_signed ? "sarq" : "shrq", 64 - bits, reg64[r]);
    }
}

/* ------------------------------------------------------------------ */
/* __int128                                                           */
/* ------------------------------------------------------------------ */

/* a in rax:rdx, b in rcx:rsi, the result in rax:rdx */
static void x64_wide_shift(IROp op) {
    int l_big = ++x64_nlabels;
    int l_done = ++x64_nlabels;
    /* the count is in cl; the double shifts take it modulo 64, so counts of 64 and up are separate code */
    x64_emit("testb $64, %%cl");
    x64_emit("jne .Lcg%d", l_big);
    if (op == IR_SHL) {
        x64_emit("shldq %%cl, %%rax, %%rdx");
        x64_emit("shlq %%cl, %%rax");
    } else {
        x64_emit("shrdq %%cl, %%rdx, %%rax");
        x64_emit(op == IR_LSHR ? "shrq %%cl, %%rdx" : "sarq %%cl, %%rdx");
    }
    x64_emit("jmp .Lcg%d", l_done);
    peep_flush();
    fprintf(x64_cg_out, ".Lcg%d:\n", l_big);
    if (op == IR_SHL) {
        x64_emit("movq %%rax, %%rdx");
        x64_emit("shlq %%cl, %%rdx");
        x64_emit("xorl %%eax, %%eax");
    } else if (op == IR_LSHR) {
        x64_emit("movq %%rdx, %%rax");
        x64_emit("shrq %%cl, %%rax");
        x64_emit("xorl %%edx, %%edx");
    } else {
        x64_emit("movq %%rdx, %%rax");
        x64_emit("sarq %%cl, %%rax");
        x64_emit("sarq $63, %%rdx");
    }
    peep_flush();
    fprintf(x64_cg_out, ".Lcg%d:\n", l_done);
}

/* set the flags for an __int128 comparison of rax:rdx with rcx:rsi; returns the condition */
static const char *x64_wide_compare(IRCond cc) {
    switch (cc) {
        case IR_EQ:
        case IR_NE:
            x64_emit("xorq %%rcx, %%rax");
            x64_emit("xorq %%rsi, %%rdx");
            x64_emit("orq %%rdx, %%rax");
            return icc(cc);
        /* the flags of the 128 bit subtraction: l and b (a borrow) are a < b, the others are the swap or the negation */
        case IR_GT:
        case IR_LE:
        case IR_UGT:
        case IR_ULE:
            x64_emit("cmpq %%rax, %%rcx");
            x64_emit("sbbq %%rdx, %%rsi");
            return cc == IR_GT ? "l" : cc == IR_LE ? "ge" : cc == IR_UGT ? "b" : "ae";
        default:
            x64_emit("cmpq %%rcx, %%rax");
            x64_emit("sbbq %%rsi, %%rdx");
            return cc == IR_LT ? "l" : cc == IR_GE ? "ge" : cc == IR_ULT ? "b" : "ae";
    }
}

static void x64_select_wide(const IRInst *i) {
    IROp op = (IROp)i->op;
    switch (op) {
        case IR_ICONST:
            if (fits_i32(i->imm)) {
                x64_emit("movq $%ld, %%rax", i->imm);
            } else {
                x64_emit("movabsq $%ld, %%rax", i->imm);
            }
            x64_emit(i->imm < 0 ? "movq $-1, %%rdx" : "xorl %%edx, %%edx");
            x64_def_pair_done(i->dst, RAX, RDX);
            return;
        case IR_MOV:
            x64_use_pair(i->a, RAX, RDX);
            x64_def_pair_done(i->dst, RAX, RDX);
            return;
        case IR_ADD:
        case IR_SUB:
        case IR_MUL:
        case IR_AND:
        case IR_OR:
        case IR_XOR:
            x64_use_pair(i->a, RAX, RDX);
            x64_use_pair(i->b, RCX, RSI);
            switch (op) {
                case IR_ADD: x64_emit("addq %%rcx, %%rax"); x64_emit("adcq %%rsi, %%rdx"); break;
                case IR_SUB: x64_emit("subq %%rcx, %%rax"); x64_emit("sbbq %%rsi, %%rdx"); break;
                case IR_MUL:
                    x64_emit("imulq %%rcx, %%rdx");         /* high: ahi*blo + alo*bhi + the high half of alo*blo */
                    x64_emit("imulq %%rax, %%rsi");
                    x64_emit("addq %%rdx, %%rsi");
                    x64_emit("mulq %%rcx");
                    x64_emit("addq %%rsi, %%rdx");
                    break;
                case IR_AND: x64_emit("andq %%rcx, %%rax"); x64_emit("andq %%rsi, %%rdx"); break;
                case IR_OR: x64_emit("orq %%rcx, %%rax"); x64_emit("orq %%rsi, %%rdx"); break;
                default: x64_emit("xorq %%rcx, %%rax"); x64_emit("xorq %%rsi, %%rdx"); break;
            }
            x64_def_pair_done(i->dst, RAX, RDX);
            return;
        case IR_SDIV:
        case IR_UDIV:
        case IR_SREM:
        case IR_UREM:
            x64_use_pair(i->a, RDI, RSI);
            x64_use_pair(i->b, RDX, RCX);
            x64_emit("call %s@PLT", op == IR_SDIV ? "__divti3" : op == IR_UDIV ? "__udivti3" : op == IR_SREM ? "__modti3" : "__umodti3");
            x64_def_pair_done(i->dst, RAX, RDX);
            return;
        case IR_SHL:
        case IR_LSHR:
        case IR_ASHR:
            x64_use_pair(i->a, RAX, RDX);
            move(RCX, use_r(i->b, RCX), IR_I64);
            x64_wide_shift(op);
            x64_def_pair_done(i->dst, RAX, RDX);
            return;
        case IR_NEG:
            x64_use_pair(i->a, RAX, RDX);
            x64_emit("negq %%rax");
            x64_emit("adcq $0, %%rdx");
            x64_emit("negq %%rdx");
            x64_def_pair_done(i->dst, RAX, RDX);
            return;
        case IR_NOT:
            x64_use_pair(i->a, RAX, RDX);
            x64_emit("notq %%rax");
            x64_emit("notq %%rdx");
            x64_def_pair_done(i->dst, RAX, RDX);
            return;
        case IR_SEXT:
        case IR_ZEXT: {
            int a = use_r(i->a, RAX);
            if (i->size == 8) {
                move(RAX, a, IR_I64);
            } else if (op == IR_SEXT) {
                x64_emit("movslq %s, %%rax", reg32[a]);
            } else {
                x64_emit("movl %s, %%eax", reg32[a]);
            }
            if (op == IR_SEXT) {
                x64_emit("movq %%rax, %%rdx");
                x64_emit("sarq $63, %%rdx");
            } else {
                x64_emit("xorl %%edx, %%edx");
            }
            x64_def_pair_done(i->dst, RAX, RDX);
            return;
        }
        case IR_LOAD: {
            char buf[64];
            const char *m = x64_mem_operand((int)(i - x64_F->insts), RCX, 0, buf, sizeof(buf));
            x64_emit("movq %s, %%rax", m);
            m = x64_mem_operand((int)(i - x64_F->insts), RCX, 8, buf, sizeof(buf));
            x64_emit("movq %s, %%rdx", m);
            x64_def_pair_done(i->dst, RAX, RDX);
            return;
        }
        case IR_STORE: {
            char buf[64];
            const char *m;
            x64_use_pair(i->b, RAX, RDX);
            m = x64_mem_operand((int)(i - x64_F->insts), RCX, 0, buf, sizeof(buf));
            x64_emit("movq %%rax, %s", m);
            m = x64_mem_operand((int)(i - x64_F->insts), RCX, 8, buf, sizeof(buf));
            x64_emit("movq %%rdx, %s", m);
            return;
        }
        default:
            break;
    }
}

/* ------------------------------------------------------------------ */
/* x87 long double                                                    */
/* ------------------------------------------------------------------ */

/* comparisons: fucomip compares st(0) with st(1) and pops; the flags are as ucomis sets them */
static void select_x87(const IRInst *i) {
    IROp op = (IROp)i->op;
    switch (op) {
        case IR_FCONST:
            if (x64_F->vregs[i->dst].offset) {
                x64_emit("movabsq $%ld, %%rax", i->imm);
                x64_emit("movq %%rax, %s", slot(i->dst));
                x64_emit("movl $%u, %s", (unsigned)i->aux, slot_at(i->dst, 8));
            }
            return;
        case IR_MOV:
            use_x87(i->a);
            def_x87_done(i->dst);
            return;
        case IR_FADD:
        case IR_FSUB:
        case IR_FMUL:
        case IR_FDIV:
            use_x87(i->b);
            use_x87(i->a);     /* a in st(0), b in st(1) */
            switch (op) {
                case IR_FADD: x64_emit("faddp %%st, %%st(1)"); break;
                case IR_FSUB: x64_emit("fsubp %%st, %%st(1)"); break;    /* st(1) = st(0) - st(1) in AT&T syntax */
                case IR_FMUL: x64_emit("fmulp %%st, %%st(1)"); break;
                default: x64_emit("fdivp %%st, %%st(1)"); break;         /* st(1) = st(0) / st(1) */
            }
            def_x87_done(i->dst);
            return;
        case IR_FNEG:
            use_x87(i->a);
            x64_emit("fchs");
            def_x87_done(i->dst);
            return;
        case IR_LOAD: {
            char buf[64];
            const char *m = x64_mem_operand((int)(i - x64_F->insts), RCX, 0, buf, sizeof(buf));
            x64_emit("fldt %s", m);
            def_x87_done(i->dst);
            return;
        }
        case IR_STORE: {
            char buf[64];
            const char *m;
            use_x87(i->b);
            m = x64_mem_operand((int)(i - x64_F->insts), RCX, 0, buf, sizeof(buf));
            x64_emit("fstpt %s", m);
            return;
        }
        case IR_FTOF:
            /* to an x87 value from float or double */
            {
                int a = x64_use_x(i->a, 0);
                x64_emit("subq $8, %%rsp");
                x64_emit("mov%s %s, (%%rsp)", fsuf((IRType)i->t2), xmm(a));
                x64_emit(i->t2 == IR_F32 ? "flds (%%rsp)" : "fldl (%%rsp)");
                x64_emit("addq $8, %%rsp");
                def_x87_done(i->dst);
            }
            return;
        case IR_ITOF: {
            /* fild reads a signed 64 bit integer; an unsigned one with the top bit set gets 2^64 added back */
            int a;
            if (i->t2 == IR_I128) {
                x64_use_pair(i->a, RDI, RSI);
                x64_emit("call %s@PLT", i->flags & IRF_SIGNED ? "__floattixf" : "__floatuntixf");
                def_x87_done(i->dst);
                return;
            }
            a = use_r(i->a, RAX);
            if (i->t2 == IR_I32) {
                x64_emit(i->flags & IRF_SIGNED ? "movslq %s, %%rax" : "movl %s, %%eax", reg32[a]);
            } else {
                move(RAX, a, IR_I64);
            }
            x64_emit("pushq %%rax");
            x64_emit("fildq (%%rsp)");
            if (!(i->flags & IRF_SIGNED) && i->t2 == IR_I64) {
                int l_done = ++x64_nlabels;
                x64_emit("testq %%rax, %%rax");
                x64_emit("jns .Lcg%d", l_done);
                x64_emit("movl $0x5f800000, (%%rsp)");    /* 2^64 as a float */
                x64_emit("fadds (%%rsp)");
                peep_flush();
                fprintf(x64_cg_out, ".Lcg%d:\n", l_done);
            }
            x64_emit("addq $8, %%rsp");
            def_x87_done(i->dst);
            return;
        }
        default:
            break;
    }
}

/* an x87 value in st(0) converted to an integer in rax (fisttp truncates and pops) */
static void x87_to_int(IRType t, int is_unsigned) {
    x64_emit("subq $16, %%rsp");
    if (is_unsigned && t == IR_I64) {
        /* values of 2^63 and up are brought below it first */
        int l_small = ++x64_nlabels;
        int l_done = ++x64_nlabels;
        x64_emit("movl $0x5f000000, 8(%%rsp)");   /* 2^63 as a float */
        x64_emit("flds 8(%%rsp)");
        x64_emit("fucomip %%st(1), %%st");         /* 2^63 against the value */
        x64_emit("ja .Lcg%d", l_small);
        x64_emit("fsubs 8(%%rsp)");
        x64_emit("fisttpq (%%rsp)");
        x64_emit("movq (%%rsp), %%rax");
        x64_emit("btcq $63, %%rax");
        x64_emit("jmp .Lcg%d", l_done);
        peep_flush();
        fprintf(x64_cg_out, ".Lcg%d:\n", l_small);
        x64_emit("fisttpq (%%rsp)");
        x64_emit("movq (%%rsp), %%rax");
        peep_flush();
        fprintf(x64_cg_out, ".Lcg%d:\n", l_done);
    } else {
        x64_emit("fisttpq (%%rsp)");
        x64_emit("movq (%%rsp), %%rax");
    }
    x64_emit("addq $16, %%rsp");
}

/* ------------------------------------------------------------------ */
/* calls                                                              */
/* ------------------------------------------------------------------ */

/* a register argument move waiting to happen: a value from a register or a slot, or an eightbyte of a record */
typedef struct Move {
    int dst;            /* the argument register (reg64 index), or -1 for an SSE one */
    int fdst;           /* the xmm register, when dst is -1 */
    int src;            /* a forwarding register, or -1 when the value is in memory */
    int v;              /* the virtual register, when the value is a scalar */
    long add;           /* and the offset into its slot (8 for the high half of an __int128) */
    int base_v;         /* a record: the virtual register holding its address, else -1 */
    size_t off;         /* and the eightbyte's offset */
    size_t len;         /* and length (1..8) */
    int is_float;       /* a scalar floating argument */
} Move;

static void emit_move(const Move *m) {
    if (m->base_v >= 0) {
        /* the record's address in rax, r10 as the piece scratch: neither is an argument register */
        move(RAX, use_r(m->base_v, RAX), IR_I64);
        if (m->dst < 0) {
            x64_emit("%s %lu(%%rax), %s", m->len > 4 ? "movq" : "movd", (unsigned long)m->off, xmm(m->fdst));
        } else {
            x64_load_word(m->dst, RAX, m->off, m->len);
        }
        return;
    }
    if (m->is_float) {
        int r = x64_use_x(m->v, m->fdst);
        if (r != m->fdst) {
            x64_emit("movaps %s, %s", xmm(r), xmm(m->fdst));
        }
        return;
    }
    if (m->src >= 0) {
        move(m->dst, m->src, IR_I64);
    } else if (m->add) {
        x64_emit("movq %s, %s", slot_at(m->v, m->add), reg64[m->dst]);
    } else {
        use_r(m->v, m->dst);
    }
}

/*
 * The register arguments. The allocator keeps a call's operands out of
 * the argument registers (src/regalloc.h), so the moves are independent;
 * the order below, r8 and r9 last and exchanged for a swap, also covers
 * sources in those two should that ever change.
 */
static void register_args(Move *moves, int n) {
    int k;
    Move *m8 = NULL;
    Move *m9 = NULL;
    for (k = 0; k < n; k++) {
        if (moves[k].dst == R8) {
            m8 = &moves[k];
        } else if (moves[k].dst == R9) {
            m9 = &moves[k];
        } else {
            emit_move(&moves[k]);
        }
    }
    if (m8 && m9 && m8->src == R9 && m9->src == R8) {
        x64_emit("xchgq %%r8, %%r9");
        return;
    }
    if (m8 && m8->src == R9) {
        emit_move(m8);
        if (m9) {
            emit_move(m9);
        }
        return;
    }
    if (m9) {
        emit_move(m9);
    }
    if (m8) {
        emit_move(m8);
    }
}

static void x64_select_call(const IRInst *ins) {
    const IRCall *c = &x64_F->calls[ins->imm];
    ArgLoc *locs = NULL;
    Move *moves = NULL;
    int nmoves = 0;
    ArgAlloc alloc;
    size_t nstack;
    size_t area;
    int k;
    size_t m;

    /* a move per eightbyte of each argument, and one for the hidden result pointer */
    moves = xmalloc(((size_t)c->nargs * 2 + 1) * sizeof(*moves));
    if (c->nargs) {
        locs = xmalloc((size_t)c->nargs * sizeof(*locs));
    }
    x64_cg_args_begin(&alloc, c->nfixed, c->sret_addr >= 0);
    for (k = 0; k < c->nargs; k++) {
        x64_cg_args_next(&alloc, &x64_F->args[c->first_arg + k].type, &locs[k]);
    }
    nstack = x64_cg_args_end(&alloc, locs, (size_t)c->nargs);
    area = (nstack + 15) & ~(size_t)15;
    if (area) {
        x64_emit("subq $%lu, %%rsp", (unsigned long)area);
    }
    /* what goes in memory */
    for (k = 0; k < c->nargs; k++) {
        const IRArg *a = &x64_F->args[c->first_arg + k];
        const ArgLoc *l = &locs[k];
        if (l->in_regs) {
            continue;
        }
        if (a->by_addr) {
            move(RSI, use_r(a->v, RSI), IR_I64);
            x64_emit("leaq %lu(%%rsp), %%rdi", (unsigned long)l->offset);
            rep_copy(a->type.size);
        } else if (a->type.kind == ARG_FLOAT) {
            if (a->type.size == 16) {
                use_x87(a->v);
                x64_emit("fstpt %lu(%%rsp)", (unsigned long)l->offset);
            } else {
                int r = x64_use_x(a->v, 0);
                x64_emit("mov%s %s, %lu(%%rsp)", fsuf(a->type.size == 4 ? IR_F32 : IR_F64), xmm(r), (unsigned long)l->offset);
            }
        } else if (a->type.size == 16) {
            x64_use_pair(a->v, RAX, RDX);
            x64_emit("movq %%rax, %lu(%%rsp)", (unsigned long)l->offset);
            x64_emit("movq %%rdx, %lu(%%rsp)", (unsigned long)(l->offset + 8));
        } else {
            /* a narrow value is canonical in its register, so the whole eightbyte is stored */
            int r = use_r(a->v, RAX);
            x64_emit("movq %s, %lu(%%rsp)", reg64[r], (unsigned long)l->offset);
        }
    }
    /* what goes in registers */
    if (c->sret_addr >= 0) {
        Move *mv = &moves[nmoves++];
        memset(mv, 0, sizeof(*mv));
        mv->dst = RDI;
        mv->src = x64_loc_reg(c->sret_addr);
        mv->v = c->sret_addr;
        mv->base_v = -1;
    }
    for (k = 0; k < c->nargs; k++) {
        const IRArg *a = &x64_F->args[c->first_arg + k];
        const ArgLoc *l = &locs[k];
        if (!l->in_regs) {
            continue;
        }
        if (a->by_addr) {
            size_t n = (a->type.size + 7) / 8;
            size_t ni = 0;
            size_t nf = 0;
            for (m = 0; m < n; m++) {
                Move *mv = &moves[nmoves++];
                memset(mv, 0, sizeof(*mv));
                mv->base_v = a->v;
                mv->off = 8 * m;
                mv->len = a->type.size - 8 * m >= 8 ? 8 : a->type.size - 8 * m;
                if ((a->type.sse >> m) & 1) {
                    mv->dst = -1;
                    mv->fdst = (int)(l->freg + nf++);
                } else {
                    mv->dst = (int)(l->reg + ni++);
                }
            }
        } else if (a->type.size == 16) {
            Move *mv = &moves[nmoves++];
            memset(mv, 0, sizeof(*mv));
            mv->base_v = -1;
            mv->src = -1;
            mv->v = a->v;
            mv->dst = (int)l->reg;
            mv = &moves[nmoves++];
            memset(mv, 0, sizeof(*mv));
            mv->base_v = -1;
            mv->src = -1;
            mv->v = a->v;
            mv->add = 8;
            mv->dst = (int)l->reg + 1;
        } else if (l->in_fregs) {
            Move *mv = &moves[nmoves++];
            memset(mv, 0, sizeof(*mv));
            mv->base_v = -1;
            mv->is_float = 1;
            mv->dst = -1;
            mv->fdst = (int)l->freg;
            mv->v = a->v;
        } else {
            Move *mv = &moves[nmoves++];
            memset(mv, 0, sizeof(*mv));
            mv->base_v = -1;
            mv->dst = (int)l->reg;
            mv->src = x64_loc_reg(a->v);
            mv->v = a->v;
        }
    }
    register_args(moves, nmoves);
    if (c->target >= 0) {
        /* the target is never in a forwarding register (src/regalloc.c), so its slot is read after the argument moves */
        move(R10, use_r(c->target, R10), IR_I64);
    }
    if (c->variadic) {
        x64_emit("movl $%lu, %%eax", (unsigned long)alloc.nfregs);    /* vector registers used */
    }
    if (c->name) {
        x64_emit("call %s@PLT", c->name);
    } else {
        x64_emit("call *%%r10");
    }
    if (area) {
        x64_emit("addq $%lu, %%rsp", (unsigned long)area);
    }
    free(locs);
    free(moves);

    /* the result */
    if (ins->dst >= 0) {
        IRType t = x64_vtype(ins->dst);
        if (t == IR_F80) {
            def_x87_done(ins->dst);
        } else if (ir_type_is_float(t)) {
            x64_def_done_x(ins->dst, 0);
        } else if (t == IR_I128) {
            x64_def_pair_done(ins->dst, RAX, RDX);
        } else {
            def_done_r(ins->dst, RAX);
        }
    } else if (c->result_addr >= 0) {
        /* each eightbyte of a record returned in registers: INTEGER ones take rax then rdx, SSE ones xmm0 then xmm1 */
        const ArgType *t = &c->ret;
        int r = use_r(c->result_addr, RDI);
        size_t n = (t->size + 7) / 8;
        size_t ni = 0;
        size_t nf = 0;
        if (t->kind == ARG_CX87) {
            x64_emit("fstpt (%s)", reg64[r]);          /* the real part is st(0), the imaginary part st(1) */
            x64_emit("fstpt 16(%s)", reg64[r]);
            return;
        }
        for (m = 0; m < n; m++) {
            if ((t->sse >> m) & 1) {
                x64_emit("movq %s, %lu(%s)", xmm((int)nf++), (unsigned long)(8 * m), reg64[r]);
            } else {
                x64_emit("movq %s, %lu(%s)", ni++ == 0 ? "%rax" : "%rdx", (unsigned long)(8 * m), reg64[r]);
            }
        }
    }
}

/* a return: the value to rax / xmm0 / st(0) / rax:rdx, or a record's eightbytes, then the epilogue */
static void x64_select_ret(const IRInst *i) {
    if (i->flags & IRF_RECORD) {
        const ArgType *t = &x64_F->argtypes[i->imm];
        int r = use_r(i->a, RDI);
        size_t n = (t->size + 7) / 8;
        size_t k;
        size_t ni = 0;
        size_t nf = 0;
        if (t->kind == ARG_CX87) {
            x64_emit("fldt 16(%s)", reg64[r]);     /* the real part ends up in st(0), the imaginary one in st(1) */
            x64_emit("fldt (%s)", reg64[r]);
        } else {
            move(RDI, r, IR_I64);
            for (k = 0; k < n; k++) {
                size_t len = t->size - 8 * k >= 8 ? 8 : t->size - 8 * k;
                if ((t->sse >> k) & 1) {
                    x64_emit("%s %lu(%%rdi), %s", len > 4 ? "movq" : "movd", (unsigned long)(8 * k), xmm((int)nf++));
                } else if (ni++ == 0) {
                    x64_load_word(RAX, RDI, 8 * k, len);
                } else {
                    x64_load_word(RDX, RDI, 8 * k, len);
                }
            }
        }
    } else if (i->a >= 0) {
        IRType t = x64_vtype(i->a);
        if (t == IR_F80) {
            use_x87(i->a);
        } else if (ir_type_is_float(t)) {
            int r = x64_use_x(i->a, 0);
            if (r != 0) {
                x64_emit("movaps %s, %%xmm0", xmm(r));
            }
        } else if (t == IR_I128) {
            x64_use_pair(i->a, RAX, RDX);
        } else {
            move(RAX, use_r(i->a, RAX), IR_I64);
        }
    }
    x64_epilogue();
}

/*
 * Variadic callees. A va_list is the System V structure
 *   { unsigned gp_offset; unsigned fp_offset; void *overflow_arg_area; void *reg_save_area; }
 * gp_offset counts the bytes of the register save area already consumed.
 * An argument of up to two eightbytes is in the save area if enough
 * registers were left for the whole of it, otherwise in the overflow
 * area; larger composites are always in the overflow area.
 */
static void x64_select_vaarg(const IRInst *i) {
    const ArgType *t = &x64_F->argtypes[i->aux];
    size_t size = t->size;
    size_t align = t->align;
    ArgKind kind = t->kind;
    size_t n = (size + 7) / 8;
    int l_overflow = ++x64_nlabels;
    int l_done = ++x64_nlabels;
    long temp = i->imm;

    move(RAX, use_r(i->a, RAX), IR_I64);
    if (kind == ARG_RECORD && size <= 16 && t->sse) {
        /*
         * A record with vector class eightbytes: each eightbyte comes from
         * its own save area when every one of them fits, gathered into
         * the temporary; otherwise the whole record is in the overflow area.
         */
        size_t nf = sse_count(t, n);
        size_t k;
        if (n > nf) {
            x64_emit("movl (%%rax), %%ecx");
            x64_emit("cmpl $%lu, %%ecx", (unsigned long)(48 - 8 * (n - nf)));
            x64_emit("ja .Lcg%d", l_overflow);
        }
        x64_emit("movl 4(%%rax), %%esi");
        x64_emit("cmpl $%lu, %%esi", (unsigned long)(176 - 16 * nf));
        x64_emit("ja .Lcg%d", l_overflow);
        x64_emit("movq 16(%%rax), %%r10");
        for (k = 0; k < n; k++) {
            if ((t->sse >> k) & 1) {
                x64_emit("movq (%%r10,%%rsi), %%rdi");
                x64_emit("addl $16, %%esi");
            } else {
                x64_emit("movq (%%r10,%%rcx), %%rdi");
                x64_emit("addl $8, %%ecx");
            }
            x64_emit("movq %%rdi, %ld(%%rbp)", temp + 8 * (long)k);
        }
        if (n > nf) {
            x64_emit("movl %%ecx, (%%rax)");
        }
        x64_emit("movl %%esi, 4(%%rax)");
        x64_emit("leaq %ld(%%rbp), %%rax", temp);
        x64_emit("jmp .Lcg%d", l_done);
    } else if (kind == ARG_FLOAT && size <= 8) {
        x64_emit("movl 4(%%rax), %%ecx");
        x64_emit("cmpl $176, %%ecx");
        x64_emit("jae .Lcg%d", l_overflow);
        x64_emit("movq 16(%%rax), %%r10");
        x64_emit("addq %%rcx, %%r10");
        x64_emit("addl $16, %%ecx");
        x64_emit("movl %%ecx, 4(%%rax)");
        x64_emit("movq %%r10, %%rax");
        x64_emit("jmp .Lcg%d", l_done);
    } else if (kind == ARG_INT || (kind == ARG_RECORD && size <= 16)) {
        x64_emit("movl (%%rax), %%ecx");
        x64_emit("cmpl $%lu, %%ecx", (unsigned long)(48 - 8 * n));
        x64_emit("ja .Lcg%d", l_overflow);
        x64_emit("movq 16(%%rax), %%r10");
        x64_emit("addq %%rcx, %%r10");
        x64_emit("addl $%lu, %%ecx", (unsigned long)(8 * n));
        x64_emit("movl %%ecx, (%%rax)");
        x64_emit("movq %%r10, %%rax");
        x64_emit("jmp .Lcg%d", l_done);
    }
    peep_flush();
    fprintf(x64_cg_out, ".Lcg%d:\n", l_overflow);
    x64_emit("movq 8(%%rax), %%rcx");
    if (align > 8) {
        x64_emit("addq $%lu, %%rcx", (unsigned long)(align - 1));
        x64_emit("andq $-%lu, %%rcx", (unsigned long)align);
    }
    x64_emit("leaq %lu(%%rcx), %%r10", (unsigned long)(8 * n));
    x64_emit("movq %%r10, 8(%%rax)");
    x64_emit("movq %%rcx, %%rax");
    peep_flush();
    fprintf(x64_cg_out, ".Lcg%d:\n", l_done);
    def_done_r(i->dst, RAX);
}

/* ------------------------------------------------------------------ */
/* the instructions                                                   */
/* ------------------------------------------------------------------ */

static void x64_branch_to(int target, int next) {
    if (target != next) {
        x64_emit("jmp %s", x64_blabel(target));
    }
}

static void x64_cond_branch(const char *cc, int bt, int bf, int next) {
    if (bf == next) {
        x64_emit("j%s %s", cc, x64_blabel(bt));
    } else if (bt == next) {
        x64_emit("j%s %s", invert_cc(cc), x64_blabel(bf));
    } else {
        x64_emit("j%s %s", cc, x64_blabel(bt));
        x64_emit("jmp %s", x64_blabel(bf));
    }
}

/* set the flags for an integer comparison; returns the condition to test */
static const char *x64_compare(const IRInst *i) {
    IRType t = (IRType)i->type;
    int a;
    if (t == IR_I128) {
        x64_use_pair(i->a, RAX, RDX);
        x64_use_pair(i->b, RCX, RSI);
        return x64_wide_compare((IRCond)i->cc);
    }
    a = use_r(i->a, RAX);
    if (i->flags & IRF_IMM) {
        if (t == IR_I32 || fits_i32(i->imm)) {
            x64_emit("cmp%c $%ld, %s", sfx(t), i->imm, rt(a, t));
        } else {
            x64_emit("movabsq $%ld, %%rcx", i->imm);
            x64_emit("cmpq %%rcx, %s", reg64[a]);
        }
    } else {
        int b = use_r(i->b, RCX);
        x64_emit("cmp%c %s, %s", sfx(t), rt(b, t), rt(a, t));
    }
    return icc((IRCond)i->cc);
}

/*
 * A floating comparison: ucomis sets CF for below and for unordered, so
 * "above" tests are false for a NaN; for = and != the parity flag has to
 * be looked at too. Returns the condition for the four that are one
 * flag, NULL for = and !=, which materialize their result in `al`.
 */
static const char *x64_fcompare(const IRInst *i, int *is_eq) {
    IRType t = (IRType)i->type;
    IRCond cc = (IRCond)i->cc;
    *is_eq = 0;
    if (t == IR_F80) {
        if (cc == IR_LT || cc == IR_LE) {
            use_x87(i->a);
            use_x87(i->b);      /* b in st(0): b > a is a < b */
        } else {
            use_x87(i->b);
            use_x87(i->a);
        }
        x64_emit("fucomip %%st(1), %%st");
        x64_emit("fstp %%st(0)");
    } else {
        int a = x64_use_x(i->a, 0);
        int b = x64_use_x(i->b, 1);
        if (cc == IR_LT || cc == IR_LE) {
            x64_emit("ucomi%s %s, %s", fsuf(t), xmm(a), xmm(b));
        } else {
            x64_emit("ucomi%s %s, %s", fsuf(t), xmm(b), xmm(a));
        }
    }
    switch (cc) {
        case IR_GT: case IR_LT: return "a";
        case IR_GE: case IR_LE: return "ae";
        case IR_EQ:
            x64_emit("sete %%al");
            x64_emit("setnp %%cl");
            x64_emit("andb %%cl, %%al");
            *is_eq = 1;
            return NULL;
        default:
            x64_emit("setne %%al");
            x64_emit("setp %%cl");
            x64_emit("orb %%cl, %%al");
            *is_eq = 1;
            return NULL;
    }
}

/* one instruction; returns 1 when it consumed the next one too (a compare fused with its branch) */
static int x64_select_inst(int index, int next_block) {
    const IRInst *i = &x64_F->insts[index];
    IRType t = (IRType)i->type;
    IROp op = (IROp)i->op;
    char buf[64];
    int a, b, d;

    if (x64_folded[index]) {
        return 0;
    }
    if (i->dst >= 0 && x64_vtype(i->dst) == IR_I128 && op != IR_CALL && op != IR_FTOI && op != IR_TRUNC) {
        x64_select_wide(i);
        return 0;
    }
    if (op == IR_STORE && t == IR_I128) {
        x64_select_wide(i);
        return 0;
    }
    if ((i->dst >= 0 && x64_vtype(i->dst) == IR_F80 && op != IR_CALL) || (op == IR_STORE && t == IR_F80)) {
        select_x87(i);
        return 0;
    }
    switch (op) {
        case IR_NOP:
            return 0;

        case IR_ICONST:
            d = x64_def_reg(i->dst, RAX);
            if (t == IR_I32) {
                x64_emit("movl $%ld, %s", i->imm, reg32[d]);
            } else if (fits_i32(i->imm)) {
                x64_emit("movq $%ld, %s", i->imm, reg64[d]);
            } else {
                x64_emit("movabsq $%ld, %s", i->imm, reg64[d]);
            }
            def_done_r(i->dst, d);
            return 0;

        case IR_FCONST:
            d = x64_def_reg(i->dst, 0);
            if (t == IR_F32) {
                x64_emit("movl $%ld, %%eax", i->imm & 0xffffffffL);
                x64_emit("movd %%eax, %s", xmm(d));
            } else {
                x64_emit("movabsq $%ld, %%rax", i->imm);
                x64_emit("movq %%rax, %s", xmm(d));
            }
            x64_def_done_x(i->dst, d);
            return 0;

        case IR_ADDR_LOCAL:
            d = x64_def_reg(i->dst, RAX);
            x64_emit("leaq %ld(%%rbp), %s", i->imm, reg64[d]);
            def_done_r(i->dst, d);
            return 0;

        case IR_ADDR_GLOBAL:
            d = x64_def_reg(i->dst, RAX);
            if (i->flags & IRF_EXTERN) {
                x64_emit("movq %s@GOTPCREL(%%rip), %s", i->sym, reg64[d]);
            } else {
                x64_emit("leaq %s(%%rip), %s", i->sym, reg64[d]);
            }
            def_done_r(i->dst, d);
            return 0;

        case IR_ADDR_STRING:
            d = x64_def_reg(i->dst, RAX);
            x64_emit("leaq .L.str.%ld(%%rip), %s", i->imm, reg64[d]);
            def_done_r(i->dst, d);
            return 0;

        case IR_MOV:
            if (x64_loc_reg(i->a) < 0 && x64_loc_reg(i->dst) < 0 && x64_F->vregs[i->a].offset == x64_F->vregs[i->dst].offset) {
                return 0;   /* the two share a slot (src/regalloc.c merged them), or the destination is never read */
            }
            if (ir_type_is_float(t)) {
                a = x64_use_x(i->a, 0);
                d = x64_def_reg(i->dst, a);
                if (d != a) {
                    x64_emit("movaps %s, %s", xmm(a), xmm(d));
                }
                x64_def_done_x(i->dst, d);
            } else {
                a = use_r(i->a, RAX);
                d = x64_def_reg(i->dst, a);
                move(d, a, IR_I64);
                def_done_r(i->dst, d);
            }
            return 0;

        case IR_LOAD: {
            const char *m = x64_mem_operand(index, RCX, 0, buf, sizeof(buf));
            if (ir_type_is_float(t)) {
                d = x64_def_reg(i->dst, 0);
                x64_emit("mov%s %s, %s", fsuf(t), m, xmm(d));
                x64_def_done_x(i->dst, d);
                return 0;
            }
            d = x64_def_reg(i->dst, RAX);
            if (t == IR_I32) {
                switch (i->size) {
                    case 1: x64_emit(i->flags & IRF_SIGNED ? "movsbl %s, %s" : "movzbl %s, %s", m, reg32[d]); break;
                    case 2: x64_emit(i->flags & IRF_SIGNED ? "movswl %s, %s" : "movzwl %s, %s", m, reg32[d]); break;
                    default: x64_emit("movl %s, %s", m, reg32[d]); break;
                }
            } else {
                switch (i->size) {
                    case 1: x64_emit(i->flags & IRF_SIGNED ? "movsbq %s, %s" : "movzbq %s, %s", m, reg64[d]); break;
                    case 2: x64_emit(i->flags & IRF_SIGNED ? "movswq %s, %s" : "movzwq %s, %s", m, reg64[d]); break;
                    case 4: x64_emit(i->flags & IRF_SIGNED ? "movslq %s, %s" : "movl %s, %s", m, i->flags & IRF_SIGNED ? reg64[d] : reg32[d]); break;
                    default: x64_emit("movq %s, %s", m, reg64[d]); break;
                }
            }
            def_done_r(i->dst, d);
            return 0;
        }

        case IR_STORE: {
            const char *m;
            if (ir_type_is_float(t)) {
                b = x64_use_x(i->b, 0);
                m = x64_mem_operand(index, RCX, 0, buf, sizeof(buf));
                x64_emit("mov%s %s, %s", fsuf(t), xmm(b), m);
            } else {
                b = use_r(i->b, RDX);
                m = x64_mem_operand(index, RCX, 0, buf, sizeof(buf));
                x64_emit("mov%c %s, %s", i->size == 1 ? 'b' : i->size == 2 ? 'w' : i->size == 4 ? 'l' : 'q', rsz(b, i->size), m);
            }
            return 0;
        }

        case IR_LOADBITS: {
            const char *m = x64_mem_operand(index, RCX, 0, buf, sizeof(buf));
            load_unit(m, i->size, RAX);
            extract_bits(RAX, (unsigned)(i->imm & 0xff), (unsigned)(i->imm >> 8), i->flags & IRF_SIGNED);
            def_done_r(i->dst, RAX);
            return 0;
        }

        case IR_STOREBITS: {
            unsigned bit_offset = (unsigned)(i->imm & 0xff);
            unsigned bits = (unsigned)(i->imm >> 8);
            unsigned long mask = bits >= 64 ? ~0ul : (1ul << bits) - 1;
            const char *m;
            move(RAX, use_r(i->b, RAX), IR_I64);
            m = x64_mem_operand(index, RCX, 0, buf, sizeof(buf));
            load_unit(m, i->size, RDX);
            x64_emit("movabsq $%ld, %%rsi", (long)~(mask << bit_offset));
            x64_emit("andq %%rsi, %%rdx");
            x64_emit("movabsq $%ld, %%rsi", (long)mask);
            x64_emit("andq %%rsi, %%rax");
            if (bit_offset) {
                x64_emit("shlq $%u, %%rax", bit_offset);
            }
            x64_emit("orq %%rdx, %%rax");
            x64_emit("mov%c %s, %s", i->size == 1 ? 'b' : i->size == 2 ? 'w' : i->size == 4 ? 'l' : 'q', rsz(RAX, i->size), m);
            if (i->dst >= 0) {
                extract_bits(RAX, bit_offset, bits, i->flags & IRF_SIGNED);
                def_done_r(i->dst, RAX);
            }
            return 0;
        }

        case IR_COPY:
            move(RSI, use_r(i->b, RSI), IR_I64);
            move(RDI, use_r(i->a, RDI), IR_I64);
            rep_copy((size_t)i->imm);
            return 0;

        case IR_ZERO:
            move(RDI, use_r(i->a, RDI), IR_I64);
            x64_emit("movq $%lu, %%rcx", (unsigned long)i->imm);
            x64_emit("xorl %%eax, %%eax");
            x64_emit("rep stosb");
            return 0;

        case IR_ADD: case IR_SUB: case IR_AND: case IR_OR: case IR_XOR: {
            const char *mn = op == IR_ADD ? "add" : op == IR_SUB ? "sub" : op == IR_AND ? "and" : op == IR_OR ? "or" : "xor";
            a = use_r(i->a, RAX);
            d = x64_def_reg(i->dst, RAX);
            if (i->flags & IRF_IMM) {
                move(d, a, t);
                if (t == IR_I32 || fits_i32(i->imm)) {
                    if (!(i->imm == 0 && (op == IR_ADD || op == IR_SUB || op == IR_OR || op == IR_XOR))) {
                        x64_emit("%s%c $%ld, %s", mn, sfx(t), i->imm, rt(d, t));
                    }
                } else {
                    x64_emit("movabsq $%ld, %%rcx", i->imm);
                    x64_emit("%sq %%rcx, %s", mn, reg64[d]);
                }
            } else {
                b = use_r(i->b, RCX);
                if (d == b && d != a) {
                    /* the destination holds b: a op b commutes, and a - b is -(b - a) */
                    if (op == IR_SUB) {
                        x64_emit("neg%c %s", sfx(t), rt(d, t));
                        x64_emit("add%c %s, %s", sfx(t), rt(a, t), rt(d, t));
                    } else {
                        x64_emit("%s%c %s, %s", mn, sfx(t), rt(a, t), rt(d, t));
                    }
                } else {
                    move(d, a, t);
                    x64_emit("%s%c %s, %s", mn, sfx(t), rt(b, t), rt(d, t));
                }
            }
            def_done_r(i->dst, d);
            return 0;
        }

        case IR_MUL:
            a = use_r(i->a, RAX);
            d = x64_def_reg(i->dst, RAX);
            if (i->flags & IRF_IMM) {
                if (t == IR_I32 || fits_i32(i->imm)) {
                    x64_emit("imul%c $%ld, %s, %s", sfx(t), i->imm, rt(a, t), rt(d, t));
                } else {
                    x64_emit("movabsq $%ld, %%rcx", i->imm);
                    move(d, a, t);
                    x64_emit("imulq %%rcx, %s", reg64[d]);
                }
            } else {
                b = use_r(i->b, RCX);
                if (d == b) {
                    x64_emit("imul%c %s, %s", sfx(t), rt(a, t), rt(d, t));
                } else {
                    move(d, a, t);
                    x64_emit("imul%c %s, %s", sfx(t), rt(b, t), rt(d, t));
                }
            }
            def_done_r(i->dst, d);
            return 0;

        case IR_SDIV: case IR_UDIV: case IR_SREM: case IR_UREM:
            move(RAX, use_r(i->a, RAX), t);
            if (i->flags & IRF_IMM) {
                if (t == IR_I32) {
                    x64_emit("movl $%ld, %%ecx", i->imm);
                } else if (fits_i32(i->imm)) {
                    x64_emit("movq $%ld, %%rcx", i->imm);
                } else {
                    x64_emit("movabsq $%ld, %%rcx", i->imm);
                }
                b = RCX;
            } else {
                b = use_r(i->b, RCX);
            }
            if (op == IR_SDIV || op == IR_SREM) {
                x64_emit(t == IR_I32 ? "cltd" : "cqto");
                x64_emit("idiv%c %s", sfx(t), rt(b, t));
            } else {
                x64_emit("xorl %%edx, %%edx");
                x64_emit("div%c %s", sfx(t), rt(b, t));
            }
            def_done_r(i->dst, op == IR_SDIV || op == IR_UDIV ? RAX : RDX);
            return 0;

        case IR_SHL: case IR_LSHR: case IR_ASHR: {
            const char *mn = op == IR_SHL ? "shl" : op == IR_LSHR ? "shr" : "sar";
            a = use_r(i->a, RAX);
            d = x64_def_reg(i->dst, RAX);
            if (i->flags & IRF_IMM) {
                move(d, a, t);
                if (i->imm & (t == IR_I32 ? 31 : 63)) {
                    x64_emit("%s%c $%ld, %s", mn, sfx(t), i->imm & (t == IR_I32 ? 31 : 63), rt(d, t));
                }
            } else {
                b = use_r(i->b, RCX);
                move(RCX, b, IR_I64);
                move(d, a, t);
                x64_emit("%s%c %%cl, %s", mn, sfx(t), rt(d, t));
            }
            def_done_r(i->dst, d);
            return 0;
        }

        case IR_NEG:
        case IR_NOT:
            a = use_r(i->a, RAX);
            d = x64_def_reg(i->dst, RAX);
            move(d, a, t);
            x64_emit("%s%c %s", op == IR_NEG ? "neg" : "not", sfx(t), rt(d, t));
            def_done_r(i->dst, d);
            return 0;

        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV: {
            const char *mn = op == IR_FADD ? "add" : op == IR_FSUB ? "sub" : op == IR_FMUL ? "mul" : "div";
            a = x64_use_x(i->a, 0);
            b = x64_use_x(i->b, 1);
            d = x64_def_reg(i->dst, 0);
            if (d == b && d != a) {
                /* the destination holds b: a op b commutes, else through xmm0 */
                if (op == IR_FADD || op == IR_FMUL) {
                    x64_emit("%s%s %s, %s", mn, fsuf(t), xmm(a), xmm(d));
                } else {
                    if (a != 0) {
                        x64_emit("movaps %s, %%xmm0", xmm(a));
                    }
                    x64_emit("%s%s %s, %%xmm0", mn, fsuf(t), xmm(b));
                    x64_emit("movaps %%xmm0, %s", xmm(d));
                }
            } else {
                if (d != a) {
                    x64_emit("movaps %s, %s", xmm(a), xmm(d));
                }
                x64_emit("%s%s %s, %s", mn, fsuf(t), xmm(b), xmm(d));
            }
            x64_def_done_x(i->dst, d);
            return 0;
        }

        case IR_FNEG:
        case IR_FABS:
            a = x64_use_x(i->a, 0);
            d = x64_def_reg(i->dst, 0);
            if (t == IR_F32) {
                x64_emit("movd %s, %%ecx", xmm(a));
                x64_emit(op == IR_FNEG ? "xorl $0x80000000, %%ecx" : "andl $0x7fffffff, %%ecx");
                x64_emit("movd %%ecx, %s", xmm(d));
            } else {
                x64_emit("movq %s, %%rcx", xmm(a));
                x64_emit(op == IR_FNEG ? "btcq $63, %%rcx" : "btrq $63, %%rcx");
                x64_emit("movq %%rcx, %s", xmm(d));
            }
            x64_def_done_x(i->dst, d);
            return 0;

        case IR_FSQRT:
            a = x64_use_x(i->a, 0);
            d = x64_def_reg(i->dst, 0);
            x64_emit("%s %s, %s", t == IR_F32 ? "sqrtss" : "sqrtsd", xmm(a), xmm(d));
            x64_def_done_x(i->dst, d);
            return 0;

        case IR_ICMP:
        case IR_FCMP: {
            const char *cc;
            int is_eq = 0;
            const IRInst *n = index + 1 < x64_F->ninsts ? &x64_F->insts[index + 1] : NULL;
            int fuse = n && n->op == IR_CONDBR && n->a == i->dst && x64_F->vregs[i->dst].nuses == 1;
            cc = op == IR_ICMP ? x64_compare(i) : x64_fcompare(i, &is_eq);
            if (is_eq) {
                /* the result is in al */
                if (fuse) {
                    x64_emit("testb %%al, %%al");
                    x64_cond_branch("ne", (int)n->imm, n->aux, next_block);
                    return 1;
                }
                x64_emit("movzbl %%al, %%eax");
                def_done_r(i->dst, RAX);
                return 0;
            }
            if (fuse) {
                x64_cond_branch(cc, (int)n->imm, n->aux, next_block);
                return 1;
            }
            d = x64_def_reg(i->dst, RAX);
            x64_emit("set%s %s", cc, reg8[d]);
            x64_emit("movzbl %s, %s", reg8[d], reg32[d]);
            def_done_r(i->dst, d);
            return 0;
        }

        case IR_SEXT:
        case IR_ZEXT:
            a = use_r(i->a, RAX);
            d = x64_def_reg(i->dst, RAX);
            if (op == IR_SEXT) {
                switch (i->size) {
                    case 1: x64_emit("movsb%c %s, %s", sfx(t), reg8[a], rt(d, t)); break;
                    case 2: x64_emit("movsw%c %s, %s", sfx(t), reg16[a], rt(d, t)); break;
                    default: x64_emit("movslq %s, %s", reg32[a], reg64[d]); break;
                }
            } else {
                switch (i->size) {
                    case 1: x64_emit("movzb%c %s, %s", sfx(t), reg8[a], rt(d, t)); break;
                    case 2: x64_emit("movzw%c %s, %s", sfx(t), reg16[a], rt(d, t)); break;
                    default: x64_emit("movl %s, %s", reg32[a], reg32[d]); break;
                }
            }
            def_done_r(i->dst, d);
            return 0;

        case IR_TRUNC:
            if (x64_vtype(i->a) == IR_I128) {
                x64_emit("movq %s, %%rax", slot(i->a));
                def_done_r(i->dst, RAX);
            } else {
                a = use_r(i->a, RAX);
                d = x64_def_reg(i->dst, a);
                move(d, a, IR_I32);
                def_done_r(i->dst, d);
            }
            return 0;

        case IR_ITOF:
            if (i->t2 == IR_I128) {
                x64_use_pair(i->a, RDI, RSI);
                x64_emit("call %s@PLT", t == IR_F32 ? (i->flags & IRF_SIGNED ? "__floattisf" : "__floatuntisf")
                                               : (i->flags & IRF_SIGNED ? "__floattidf" : "__floatuntidf"));
                x64_def_done_x(i->dst, 0);
                return 0;
            }
            a = use_r(i->a, RAX);
            d = x64_def_reg(i->dst, 0);
            if (i->t2 == IR_I32) {
                if (i->flags & IRF_SIGNED) {
                    x64_emit("cvtsi2%sl %s, %s", fsuf(t), reg32[a], xmm(d));
                } else {
                    x64_emit("movl %s, %%eax", reg32[a]);
                    x64_emit("cvtsi2%sq %%rax, %s", fsuf(t), xmm(d));
                }
            } else if (i->flags & IRF_SIGNED) {
                x64_emit("cvtsi2%sq %s, %s", fsuf(t), reg64[a], xmm(d));
            } else {
                /* a value with the top bit set is halved, converted and doubled, keeping the lost bit for rounding */
                int l_neg = ++x64_nlabels;
                int l_done = ++x64_nlabels;
                move(RAX, a, IR_I64);
                x64_emit("testq %%rax, %%rax");
                x64_emit("js .Lcg%d", l_neg);
                x64_emit("cvtsi2%sq %%rax, %s", fsuf(t), xmm(d));
                x64_emit("jmp .Lcg%d", l_done);
                peep_flush();
                fprintf(x64_cg_out, ".Lcg%d:\n", l_neg);
                x64_emit("movq %%rax, %%rcx");
                x64_emit("shrq $1, %%rcx");
                x64_emit("andl $1, %%eax");
                x64_emit("orq %%rax, %%rcx");
                x64_emit("cvtsi2%sq %%rcx, %s", fsuf(t), xmm(d));
                x64_emit("add%s %s, %s", fsuf(t), xmm(d), xmm(d));
                peep_flush();
                fprintf(x64_cg_out, ".Lcg%d:\n", l_done);
            }
            x64_def_done_x(i->dst, d);
            return 0;

        case IR_FTOI: {
            IRType ft = (IRType)i->t2;
            if (t == IR_I128) {
                if (ft == IR_F80) {
                    /* the long double argument goes in memory, at the aligned stack pointer */
                    use_x87(i->a);
                    x64_emit("subq $16, %%rsp");
                    x64_emit("fstpt (%%rsp)");
                    x64_emit("call %s@PLT", i->flags & IRF_SIGNED ? "__fixxfti" : "__fixunsxfti");
                    x64_emit("addq $16, %%rsp");
                } else {
                    a = x64_use_x(i->a, 0);
                    if (a != 0) {
                        x64_emit("movaps %s, %%xmm0", xmm(a));
                    }
                    x64_emit("call %s@PLT", ft == IR_F32 ? (i->flags & IRF_SIGNED ? "__fixsfti" : "__fixunssfti")
                                                     : (i->flags & IRF_SIGNED ? "__fixdfti" : "__fixunsdfti"));
                }
                x64_def_pair_done(i->dst, RAX, RDX);
                return 0;
            }
            if (ft == IR_F80) {
                use_x87(i->a);
                x87_to_int(t, !(i->flags & IRF_SIGNED));
                def_done_r(i->dst, RAX);
                return 0;
            }
            a = x64_use_x(i->a, 0);
            d = x64_def_reg(i->dst, RAX);
            if (t == IR_I32) {
                /* an unsigned int converts through the 64 bit form, whose low half is the answer */
                x64_emit("cvtt%s2si%c %s, %s", fsuf(ft), i->flags & IRF_SIGNED ? 'l' : 'q', xmm(a), i->flags & IRF_SIGNED ? reg32[d] : reg64[d]);
            } else if (i->flags & IRF_SIGNED) {
                x64_emit("cvtt%s2siq %s, %s", fsuf(ft), xmm(a), reg64[d]);
            } else {
                /* values of 2^63 and up are converted below that and the top bit put back */
                int l_big = ++x64_nlabels;
                int l_done = ++x64_nlabels;
                if (a != 0) {
                    x64_emit("movaps %s, %%xmm0", xmm(a));
                }
                if (ft == IR_F32) {
                    x64_emit("movl $0x5f000000, %%ecx");
                    x64_emit("movd %%ecx, %%xmm1");
                } else {
                    x64_emit("movabsq $0x43e0000000000000, %%rcx");
                    x64_emit("movq %%rcx, %%xmm1");
                }
                x64_emit("ucomi%s %%xmm1, %%xmm0", fsuf(ft));
                x64_emit("jae .Lcg%d", l_big);
                x64_emit("cvtt%s2siq %%xmm0, %%rax", fsuf(ft));
                x64_emit("jmp .Lcg%d", l_done);
                peep_flush();
                fprintf(x64_cg_out, ".Lcg%d:\n", l_big);
                x64_emit("sub%s %%xmm1, %%xmm0", fsuf(ft));
                x64_emit("cvtt%s2siq %%xmm0, %%rax", fsuf(ft));
                x64_emit("btcq $63, %%rax");
                peep_flush();
                fprintf(x64_cg_out, ".Lcg%d:\n", l_done);
                d = RAX;
            }
            def_done_r(i->dst, d);
            return 0;
        }

        case IR_FTOF: {
            IRType ft = (IRType)i->t2;
            if (ft == IR_F80) {
                /* an x87 value to float or double, through memory */
                use_x87(i->a);
                x64_emit("subq $8, %%rsp");
                x64_emit(t == IR_F32 ? "fstps (%%rsp)" : "fstpl (%%rsp)");
                d = x64_def_reg(i->dst, 0);
                x64_emit("mov%s (%%rsp), %s", fsuf(t), xmm(d));
                x64_emit("addq $8, %%rsp");
                x64_def_done_x(i->dst, d);
                return 0;
            }
            a = x64_use_x(i->a, 0);
            d = x64_def_reg(i->dst, 0);
            x64_emit(t == IR_F64 ? "cvtss2sd %s, %s" : "cvtsd2ss %s, %s", xmm(a), xmm(d));
            x64_def_done_x(i->dst, d);
            return 0;
        }

        case IR_BITCAST:
            if (ir_type_is_float(t)) {
                a = use_r(i->a, RAX);
                d = x64_def_reg(i->dst, 0);
                x64_emit(t == IR_F32 ? "movd %s, %s" : "movq %s, %s", t == IR_F32 ? reg32[a] : reg64[a], xmm(d));
                x64_def_done_x(i->dst, d);
            } else {
                a = x64_use_x(i->a, 0);
                d = x64_def_reg(i->dst, RAX);
                x64_emit(t == IR_I32 ? "movd %s, %s" : "movq %s, %s", xmm(a), rt(d, t));
                def_done_r(i->dst, d);
            }
            return 0;

        case IR_BSWAP:
            a = use_r(i->a, RAX);
            d = x64_def_reg(i->dst, RAX);
            move(d, a, IR_I64);
            switch (i->size) {
                case 2: x64_emit("rolw $8, %s", reg16[d]); break;
                case 4: x64_emit("bswapl %s", reg32[d]); break;
                default: x64_emit("bswapq %s", reg64[d]); break;
            }
            def_done_r(i->dst, d);
            return 0;

        case IR_CLZ:
            /* bsr/bsf are baseline x86_64 (lzcnt/tzcnt are not); like the builtins, 0 is undefined */
            a = use_r(i->a, RAX);
            d = x64_def_reg(i->dst, RAX);
            x64_emit("bsr%c %s, %s", sfx(t), rt(a, t), rt(d, t));
            x64_emit("xor%c $%d, %s", sfx(t), t == IR_I32 ? 31 : 63, rt(d, t));
            def_done_r(i->dst, d);
            return 0;

        case IR_CTZ:
            a = use_r(i->a, RAX);
            d = x64_def_reg(i->dst, RAX);
            x64_emit("bsf%c %s, %s", sfx(t), rt(a, t), rt(d, t));
            def_done_r(i->dst, d);
            return 0;

        case IR_FENCE:
            x64_emit("mfence");
            return 0;

        case IR_ALLOCA:
            move(RAX, use_r(i->a, RAX), IR_I64);
            x64_emit("addq $15, %%rax");
            x64_emit("andq $-16, %%rax");
            x64_emit("subq %%rax, %%rsp");
            d = x64_def_reg(i->dst, RAX);
            x64_emit("movq %%rsp, %s", reg64[d]);
            def_done_r(i->dst, d);
            return 0;

        case IR_BR:
            x64_branch_to((int)i->imm, next_block);
            return 0;

        case IR_CONDBR:
            a = use_r(i->a, RAX);
            x64_emit("test%c %s, %s", sfx(t), rt(a, t), rt(a, t));
            x64_cond_branch("ne", (int)i->imm, i->aux, next_block);
            return 0;

        case IR_RET:
            x64_select_ret(i);
            return 0;

        case IR_UNREACHABLE:
            x64_emit("ud2");
            return 0;

        case IR_SWITCH: {
            /* a jump table in .text: each entry the distance from the table to its case */
            const IRSwitch *sw = &x64_F->switches[i->imm];
            int k, l = ++x64_nlabels;
            a = use_r(i->a, RAX);
            x64_emit("cmpq $%d, %s", sw->n, reg64[a]);
            x64_emit("jae %s", x64_blabel(i->aux));
            x64_emit("leaq .Lcg%d(%%rip), %%rcx", l);
            x64_emit("movslq (%%rcx,%s,4), %%rax", reg64[a]);
            x64_emit("addq %%rcx, %%rax");
            x64_emit("jmp *%%rax");
            peep_flush();
            fprintf(x64_cg_out, ".Lcg%d:\n", l);
            for (k = 0; k < sw->n; k++) {
                fprintf(x64_cg_out, "\t.long %s-.Lcg%d\n", x64_blabel(x64_F->jt[sw->first + k]), l);
            }
            return 0;
        }

        case IR_CALL:
            x64_select_call(i);
            return 0;

        case IR_VASTART:
            a = use_r(i->a, RAX);
            x64_emit("movl $%lu, (%s)", (unsigned long)(8 * x64_F->nreg_params), reg64[a]);
            x64_emit("movl $%lu, 4(%s)", (unsigned long)(48 + 16 * x64_F->nfreg_params), reg64[a]);
            x64_emit("leaq %ld(%%rbp), %%rcx", x64_cg_stack_param_offset(x64_F->nstack_params));
            x64_emit("movq %%rcx, 8(%s)", reg64[a]);
            x64_emit("leaq %ld(%%rbp), %%rcx", va_save_off);
            x64_emit("movq %%rcx, 16(%s)", reg64[a]);
            return 0;

        case IR_VAARG:
            x64_select_vaarg(i);
            return 0;

        default:
            return 0;
    }
}

/* the prologue: the frame, the variadic save area, then the parameters that arrived in registers to where they live */
static void x64_prologue(void) {
    int k;
    size_t m;

    x64_emit(".text");
    if (x64_F->is_global) {
        x64_emit(".globl %s", x64_F->name);
    }
    x64_emit(".type %s, @function", x64_F->name);
    peep_flush();
    fprintf(x64_cg_out, "%s:\n", x64_F->name);
    x64_emit(".cfi_startproc");
    x64_emit("pushq %%rbp");
    x64_emit("movq %%rsp, %%rbp");
    if (x64_F->frame_size) {
        x64_emit("subq $%lu, %%rsp", (unsigned long)x64_F->frame_size);
    }
    x64_saved_regs(0);
    if (x64_F->is_variadic) {
        /*
         * The register save area: the six integer argument registers,
         * then the eight vector registers (when the caller said in al
         * that it used any), where va_arg finds the arguments that did
         * not go on the stack.
         */
        int l = ++x64_nlabels;
        va_save_off = -x64_F->frame_size - 176;
        x64_emit("subq $176, %%rsp");
        for (m = 0; m < 6; m++) {
            x64_emit("movq %s, %ld(%%rbp)", reg64[m], va_save_off + 8 * (long)m);
        }
        x64_emit("testb %%al, %%al");
        x64_emit("je .Lcg%d", l);
        for (m = 0; m < 8; m++) {
            x64_emit("movaps %%xmm%lu, %ld(%%rbp)", (unsigned long)m, va_save_off + 48 + 16 * (long)m);
        }
        peep_flush();
        fprintf(x64_cg_out, ".Lcg%d:\n", l);
    }
    if (x64_F->sret_off) {
        x64_emit("movq %%rdi, %ld(%%rbp)", x64_F->sret_off);
    }
    for (k = 0; k < x64_F->nparams; k++) {
        const IRParam *p = &x64_F->params[k];
        const ArgLoc *l = &p->loc;
        if (!l->in_regs) {
            continue;
        }
        if (p->kr_from) {
            /* a K&R float parameter: the double it arrived as, to its slot; the entry block converts it */
            x64_emit("movsd %s, %ld(%%rbp)", xmm((int)l->freg), p->kr_from);
        } else if (p->vreg >= 0) {
            if (p->is_float) {
                x64_def_done_x(p->vreg, (int)l->freg);
            } else if (p->wide) {
                x64_def_pair_done(p->vreg, (int)l->reg, (int)l->reg + 1);
            } else {
                if (p->size < 4) {
                    /* the caller may not have extended a narrow argument */
                    x64_emit("%s %s, %s", p->size == 1 ? (p->is_signed ? "movsbl" : "movzbl") : (p->is_signed ? "movswl" : "movzwl"),
                         rsz((int)l->reg, p->size), reg32[l->reg]);
                }
                def_done_r(p->vreg, (int)l->reg);
            }
        } else if (l->by_ref) {
            x64_emit("movq %s, %ld(%%rbp)", reg64[l->reg], p->offset);
        } else if (p->by_addr) {
            /* eightbytes, each from its own register of its class */
            size_t ni = 0;
            size_t nf = 0;
            for (m = 0; m < l->nregs; m++) {
                long off = p->offset + 8 * (long)m;
                if ((l->type.sse >> m) & 1) {
                    x64_emit("movq %s, %ld(%%rbp)", xmm((int)(l->freg + nf++)), off);
                } else {
                    x64_emit("movq %s, %ld(%%rbp)", reg64[l->reg + ni++], off);
                }
            }
        } else if (p->wide) {
            x64_emit("movq %s, %ld(%%rbp)", reg64[l->reg], p->offset);
            x64_emit("movq %s, %ld(%%rbp)", reg64[l->reg + 1], p->offset + 8);
        } else if (p->is_float) {
            x64_emit("mov%s %s, %ld(%%rbp)", p->size == 4 ? "ss" : "sd", xmm((int)l->freg), p->offset);
        } else {
            x64_emit("mov%c %s, %ld(%%rbp)", p->size == 1 ? 'b' : p->size == 2 ? 'w' : p->size == 4 ? 'l' : 'q', rsz((int)l->reg, p->size), p->offset);
        }
    }
}

/* a block that is nothing but a jump is skipped by the branches to it, which leaves it unreachable and unemitted */
static void x64_thread_jumps(void) {
    int b, i, k;
    for (i = 0; i < x64_F->ninsts; i++) {
        IRInst *t = &x64_F->insts[i];
        for (k = 0; k < ir_nsuccs(x64_F, t); k++) {
            int hops = 0;
            b = ir_succ(x64_F, t, k);
            while (hops++ < 64 && x64_F->blocks[b].start >= 0 && x64_F->blocks[b].end == x64_F->blocks[b].start + 1
                    && x64_F->insts[x64_F->blocks[b].start].op == IR_BR && (int)x64_F->insts[x64_F->blocks[b].start].imm != b) {
                b = (int)x64_F->insts[x64_F->blocks[b].start].imm;
            }
            ir_set_succ(x64_F, t, k, b);
        }
    }
}

static void x64_cg_select(IRFunc *f) {
    int b, i;
    int *next_of;

    x64_F = f;
    x64_fold_of = xmalloc((size_t)(f->ninsts + 1) * sizeof(int));
    x64_folded = xmalloc((size_t)(f->ninsts + 1) * sizeof(int));
    x64_npreds = xmalloc((size_t)(f->nblocks + 1) * sizeof(int));
    next_of = xmalloc((size_t)(f->nblocks + 1) * sizeof(int));
    x64_thread_jumps();
    x64_find_folds();
    regalloc(f, &x64_regtarget);
    for (b = 0; b < f->nblocks; b++) {
        x64_npreds[b] = 0;
    }
    for (b = 0; b < f->nblocks; b++) {
        const IRInst *t;
        if (f->blocks[b].start < 0 || f->blocks[b].end <= f->blocks[b].start) {
            continue;
        }
        t = &f->insts[f->blocks[b].end - 1];
        for (i = 0; i < ir_nsuccs(f, t); i++) {
            x64_npreds[ir_succ(f, t, i)]++;
        }
    }
    {
        int next = -1;
        for (b = f->nblocks - 1; b >= 0; b--) {
            next_of[b] = next;
            if (f->blocks[b].start >= 0 && (b == 0 || x64_npreds[b] > 0)) {
                next = b;
            }
        }
    }
    x64_label_base = x64_nlabels + 1;
    x64_nlabels += f->nblocks;

    x64_prologue();
    for (b = 0; b < f->nblocks; b++) {
        const IRBlock *blk = &f->blocks[b];
        if (blk->start < 0 || (b != 0 && x64_npreds[b] == 0)) {
            continue;
        }
        if (b != 0 || x64_npreds[b] > 0) {
            peep_flush();
            fprintf(x64_cg_out, "%s:\n", x64_blabel(b));
        }
        for (i = blk->start; i < blk->end; i++) {
            i += x64_select_inst(i, next_of[b]);
        }
    }
    x64_emit(".cfi_endproc");
    x64_emit(".size %s, .-%s", f->name, f->name);
    free(x64_fold_of);
    free(x64_folded);
    free(x64_npreds);
    free(next_of);
    x64_fold_of = x64_folded = x64_npreds = NULL;
    x64_F = NULL;
}

/* preprocessing for x86_64 Linux: gcc's predefined macros and the system's headers */

#include "cpp.h"
#include "predef_x86_64_linux.h"

/* KCC_X86_64_LINUX_GCC_DIR is gcc's own directory, /usr/lib/gcc/<triple>/<version> */
static const char *const x64_system_dirs[] = {
    "=/usr/include",
    KCC_X86_64_LINUX_GCC_DIR "/include",   /* gcc's own headers, after the libc's as the distribution orders them */
    NULL
};

static const char *const x64_has_builtins[] = {
    "__has_attribute", "__has_builtin", "__has_cpp_attribute", "__has_c_attribute",
    "__has_include", "__has_include_next",
    NULL
};

static const CppTarget x64_cpp_target = {
    KCC_X86_64_LINUX_SYSROOT,
    x64_system_dirs,
    x64_predefined_macros,
    x64_nonstrict_macros,
    x64_has_answers,
    x64_has_builtins
};

#include "target.h"

static const char *const x64_aliases[] = {
    "x86_64-linux", "x86_64-alpine-linux-musl", "x86_64", "amd64", NULL
};

/* the primitives, in the order of struct Backend */
static const Backend x64_backend = {
    x64_cg_begin,
    x64_cg_end,
    x64_cg_data_begin,
    x64_cg_data_int,
    x64_cg_data_addr,
    x64_cg_data_string_addr,
    x64_cg_data_bytes,
    x64_cg_data_zero,
    x64_cg_string,
    x64_cg_stack_param_offset,
    x64_cg_args_begin,
    x64_cg_args_next,
    x64_cg_args_end,
    x64_cg_record_ret_regs,
    x64_cg_select
};

/* the static link line of Alpine's gcc, less the stack protector library */
static const char *const link_startfiles[] = {
    "=/usr/lib/crt1.o", "=/usr/lib/crti.o", KCC_X86_64_LINUX_GCC_DIR "/crtbeginT.o", NULL
};
static const char *const link_lib_dirs[] = { "=/usr/lib", KCC_X86_64_LINUX_GCC_DIR, NULL };
static const char *const link_endfiles[] = {
    "--start-group", "-lgcc", "-lgcc_eh", "-lc", "--end-group",
    KCC_X86_64_LINUX_GCC_DIR "/crtend.o", "=/usr/lib/crtn.o", NULL
};
/* the dynamic one: musl's loader and libc.so (the sysroot's symlink to it), libgcc still static */
static const char *const dyn_startfiles[] = {
    "=/usr/lib/crt1.o", "=/usr/lib/crti.o", KCC_X86_64_LINUX_GCC_DIR "/crtbegin.o", NULL
};
static const char *const dyn_endfiles[] = {
    "-lc", "-lgcc", KCC_X86_64_LINUX_GCC_DIR "/crtend.o", "=/usr/lib/crtn.o", NULL
};
static const TargetLink x64_link = {
    "elf_x86_64", link_startfiles, link_lib_dirs, link_endfiles,
    dyn_startfiles, dyn_endfiles, "/lib/ld-musl-x86_64.so.1"
};

const Target target_x86_64_linux = { "x86_64-linux-musl", x64_aliases, &x64_info, &x64_cpp_target, &x64_backend, &x64_link, &as_x86_64 };
