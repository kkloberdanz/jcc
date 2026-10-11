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
 * AArch64 (arm64) backend, emitting assembly for the macOS (Mach-O)
 * toolchain. Symbols are prefixed with an underscore and the program
 * entry point is _main, which is called by the C runtime from libSystem.
 *
 * Instruction selection over the IR (src/ir.h). The allocator
 * (src/regalloc.c) puts a virtual register in a machine register (x0-x7
 * and d0-d7 for values no call sequence overlaps, x12-x15 and d16-d27
 * for values not live across a call, the callee-saved x19-x28 and
 * d8-d15 for the rest) or in a frame slot below x29. An instruction
 * reads a slot operand into a scratch register (x9, x10, x11, and
 * x1-x4 for __int128 pairs; d28-d30 for floating point), computes, and
 * stores a slot result; x16 and x17 are address and data scratch, x8
 * carries a hidden result pointer. x29 is the frame pointer; sp stays
 * 16 byte aligned.
 *
 * The statics the two backends define under the same name (the arch.h
 * implementation and the helpers both need) carry the a64_ prefix here
 * and x64_ in src/arch/arch.x86_64.c, so that both can live in one
 * translation unit, the amalgamation (tools/amalgamate.sh);
 * tests/statics.sh checks.
 */

/* Apple arm64: LP64, long double is a plain double, va_list is a char * */
static const ArchInfo a64_info = {
    "arm64",
    1,          /* char is signed */
    2, 4, 8, 8, /* short, int, long, long long */
    16, 16,     /* __int128 */
    8,          /* pointers */
    4, 8,       /* float, double */
    8, 8,       /* long double */
    8, 8,       /* va_list */
    0,          /* a char * */
    0,          /* long double is a double */
    1           /* frintm, frintp, frintz, frinta */
};

static FILE *a64_cg_out = NULL;

static int a64_nlabels = 0;     /* backend-private labels, Lcg<n>, and the block labels L<n> */

static int a64_peep_rules(void);

static void a64_emit(const char *fmt, ...) {
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

/* "OP R, [x29, #off]" or "OP R, [sp, #off]": a load or store of a frame slot; `base` tells which */
static int parse_frame_access(const char *l, char *op, char *reg, char *base, long *off) {
    size_t n = strlen(l);
    char extra;
    if (n < 2 || l[n - 1] != ']') {
        return 0;
    }
    if (sscanf(l, "\t%7s %7[wxsd0-9], [x29, #%ld%c", op, reg, off, &extra) == 4 && extra == ']') {
        *base = 'x';
    } else if (sscanf(l, "\t%7s %7[wxsd0-9], [sp, #%ld%c", op, reg, off, &extra) == 4 && extra == ']') {
        *base = 's';
    } else {
        return 0;
    }
    return strcmp(op, "ldr") == 0 || strcmp(op, "str") == 0;
}

static int a64_peep_rules(void) {
    int n = peep_n;
    const char *with[2];
    char buf[256];
    char sop[8], sreg[8], op[8], reg[8];
    char sbase, base;
    long soff, off;

    /* a store of a register to a frame slot, then a load of the slot: the value is still in the register */
    if (n >= 2 && parse_frame_access(peep_lines[n - 2], sop, sreg, &sbase, &soff) && sop[0] == 's'
            && parse_frame_access(peep_lines[n - 1], op, reg, &base, &off) && op[0] == 'l' && off == soff && base == sbase
            && sreg[0] == reg[0]) {
        if (strcmp(sreg, reg) == 0) {
            peep_replace(n - 1, 1, with, 0);
        } else {
            snprintf(buf, sizeof(buf), "\t%s %s, %s", reg[0] == 'x' || reg[0] == 'w' ? "mov" : "fmov", reg, sreg);
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

static int a64_log2_of(size_t n) {
    int b = 0;
    while (n > 1) {
        n >>= 1;
        b++;
    }
    return b;
}

static size_t a64_round_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

/* register names; the buffers rotate so that several can be live in one emit */
static const char *regname(char kind, int n) {
    static char bufs[8][8];
    static int next = 0;
    char *b = bufs[next++ & 7];
    snprintf(b, 8, "%c%d", kind, n);
    return b;
}

static const char *xn(int r) { return regname('x', r); }
static const char *wn(int r) { return regname('w', r); }
static const char *dn(int r) { return regname('d', r); }
static const char *sn(int r) { return regname('s', r); }

/* the integer register name for a type: w for i32, x otherwise */
static const char *rn(int r, IRType t) {
    return t == IR_I32 ? wn(r) : xn(r);
}

/* the floating register name for a type */
static const char *fn(int r, IRType t) {
    return t == IR_F32 ? sn(r) : dn(r);
}

/* any 64 bit constant in at most four instructions, movz/movn then movk */
static void load_imm(const char *reg, unsigned long v) {
    int i;
    int nz = 0;
    int nf = 0;
    int first = 1;

    if (v <= 0xffff) {
        a64_emit("mov %s, #%lu", reg, v);
        return;
    }
    for (i = 0; i < 4; i++) {
        unsigned h = (unsigned)((v >> (16 * i)) & 0xffff);
        if (h != 0) {
            nz++;
        }
        if (h != 0xffff) {
            nf++;
        }
    }
    if (nz <= nf) {
        for (i = 0; i < 4; i++) {
            unsigned h = (unsigned)((v >> (16 * i)) & 0xffff);
            if (h == 0) {
                continue;
            }
            a64_emit("%s %s, #0x%x, lsl #%d", first ? "movz" : "movk", reg, h, 16 * i);
            first = 0;
        }
    } else {
        for (i = 0; i < 4; i++) {
            unsigned h = (unsigned)((v >> (16 * i)) & 0xffff);
            if (h == 0xffff) {
                continue;
            }
            if (first) {
                a64_emit("movn %s, #0x%x, lsl #%d", reg, h ^ 0xffff, 16 * i);
            } else {
                a64_emit("movk %s, #0x%x, lsl #%d", reg, h, 16 * i);
            }
            first = 0;
        }
        if (first) {
            a64_emit("movn %s, #0", reg);
        }
    }
}

/* reg = x29 + offset; uses x16 when the offset is too large for an immediate (so reg may be x17, never x16 then) */
static void fp_addr(const char *reg, long offset) {
    if (offset >= 0 && offset <= 4095) {
        a64_emit("add %s, x29, #%ld", reg, offset);
    } else if (offset < 0 && offset >= -4095) {
        a64_emit("sub %s, x29, #%ld", reg, -offset);
    } else {
        load_imm("x16", (unsigned long)(offset < 0 ? -offset : offset));
        a64_emit("%s %s, x29, x16", offset < 0 ? "sub" : "add", reg);
    }
}

static void adjust_sp(const char *op, size_t bytes) {
    if (bytes == 0) {
        return;
    }
    if (bytes <= 4095) {
        a64_emit("%s sp, sp, #%lu", op, (unsigned long)bytes);
    } else {
        load_imm("x16", bytes);
        a64_emit("%s sp, sp, x16", op);
    }
}

/* can [base, #off] be encoded for an access of `size` bytes? unscaled -256..255, or scaled positive */
static int off_encodable(long off, int size) {
    if (off >= -256 && off <= 255) {
        return 1;
    }
    return off >= 0 && off % size == 0 && off / size <= 4095;
}

/* the same for ldp/stp of two 8 byte registers */
static int pair_off_encodable(long off) {
    return off >= -512 && off <= 504 && off % 8 == 0;
}

static long frame_size = 0;     /* the function's, once known: sp = x29 - frame_size - sp_disp */
static long sp_disp = 0;        /* the outgoing argument area of the call being emitted */
static int sp_known = 0;        /* no alloca in the function: a frame slot can be addressed from sp */

/*
 * The operand of a `size` byte access at x29 + offset: "x29, #offset"
 * when that encodes, else "sp, #disp" (a negative offset too far for a
 * signed 9 bit immediate is a positive scaled one from sp, when sp is
 * where the prologue left it), else through x17, so that a value on its
 * way to or from a slot can sit in x16 (a64_load_word and the copies use x17
 * only between instructions that do not touch the frame).
 */
static const char *frame_operand(long offset, int size, char *buf, size_t n) {
    long disp = offset + frame_size + sp_disp;
    if (off_encodable(offset, size)) {
        snprintf(buf, n, "x29, #%ld", offset);
    } else if (sp_known && disp >= 0 && disp % size == 0 && disp / size <= 4095) {
        snprintf(buf, n, "sp, #%ld", disp);
    } else {
        fp_addr("x17", offset);
        snprintf(buf, n, "x17");
    }
    return buf;
}

/* store the low `size` bytes of x<n> at [addr] */
static void store_sized(int n, size_t size, const char *addr) {
    switch (size) {
        case 1: a64_emit("strb w%d, [%s]", n, addr); break;
        case 2: a64_emit("strh w%d, [%s]", n, addr); break;
        case 4: a64_emit("str w%d, [%s]", n, addr); break;
        default: a64_emit("str x%d, [%s]", n, addr); break;
    }
}

/* x<n> = the `size` bytes at [addr], sign or zero extended to 64 bits */
static void load_sized(int n, size_t size, int is_signed, const char *addr) {
    switch (size) {
        case 1: a64_emit(is_signed ? "ldrsb x%d, [%s]" : "ldrb w%d, [%s]", n, addr); break;
        case 2: a64_emit(is_signed ? "ldrsh x%d, [%s]" : "ldrh w%d, [%s]", n, addr); break;
        case 4: a64_emit(is_signed ? "ldrsw x%d, [%s]" : "ldr w%d, [%s]", n, addr); break;
        default: a64_emit("ldr x%d, [%s]", n, addr); break;
    }
}

/* reg = the address of a global, through the GOT when it is defined elsewhere */
static void global_addr(const char *reg, const char *name, int is_extern) {
    if (is_extern) {
        a64_emit("adrp %s, _%s@GOTPAGE", reg, name);
        a64_emit("ldr %s, [%s, _%s@GOTPAGEOFF]", reg, reg, name);
    } else {
        a64_emit("adrp %s, _%s@PAGE", reg, name);
        a64_emit("add %s, %s, _%s@PAGEOFF", reg, reg, name);
    }
}

/*
 * Is `v` a bitmask immediate: one run of ones, possibly rotated, repeated
 * across the register with a period of 2 to 64 bits (and neither all
 * zeros nor all ones)?
 */
static int is_logical_imm(unsigned long v) {
    int period;
    if (v == 0 || v == ~0ul) {
        return 0;
    }
    for (period = 2; period <= 64; period *= 2) {
        unsigned long mask = period == 64 ? ~0ul : (1ul << period) - 1;
        unsigned long elem = v & mask;
        unsigned long rep = 0;
        unsigned long run;
        int i;
        for (i = 0; i < 64; i += period) {
            rep |= elem << i;
        }
        if (rep != v) {
            continue;
        }
        run = elem;
        if ((run & 1) && ((run >> (period - 1)) & 1)) {
            run = ~run & mask;      /* the ones wrap around: the zeros are the contiguous run */
        }
        /* adding the lowest set bit to a contiguous run clears it all */
        return run != 0 && (run & (run + (run & (0 - run)))) == 0;
    }
    return 0;
}

/* the same for a 32 bit operation: the 32 bit pattern replicated */
static int is_logical_imm32(unsigned long v) {
    v &= 0xffffffffUL;
    return is_logical_imm(v | (v << 32));
}

static void a64_cg_begin(FILE *out) {
    a64_cg_out = out;
    a64_nlabels = 0;
    peep_begin(out, a64_peep_rules, getenv("JCC_NOPEEP") == NULL);
}

static void a64_cg_end(void) {
    peep_flush();
    fputc('\n', a64_cg_out);
}

/* ------------------------------------------------------------------ */
/* data                                                               */
/* ------------------------------------------------------------------ */

static void a64_cg_data_begin(const char *name, int is_global, size_t align, int zero) {
    a64_emit(zero ? ".bss" : ".data");
    if (is_global) {
        a64_emit(".globl _%s", name);
    }
    if (align > 1) {
        a64_emit(".p2align %d", a64_log2_of(align));
    }
    peep_flush();
    fprintf(a64_cg_out, "_%s:\n", name);
}

static void a64_cg_data_int(size_t size, long value) {
    switch (size) {
        case 1: a64_emit(".byte %ld", value & 0xff); break;
        case 2: a64_emit(".short %ld", value & 0xffff); break;
        case 4: a64_emit(".long %ld", value & 0xffffffff); break;
        case 16: a64_emit(".quad %ld", value); a64_emit(".quad %d", value < 0 ? -1 : 0); break;
        default: a64_emit(".quad %ld", value); break;
    }
}

static void a64_cg_data_addr(const char *name, long addend) {
    a64_emit(".quad _%s%+lld", name, addend);
}

static void a64_cg_data_string_addr(int label, long addend) {
    a64_emit(".quad L.str.%d%+lld", label, addend);
}

static void a64_cg_data_bytes(const char *bytes, size_t len) {
    size_t i;
    peep_flush();
    fputs("\t.ascii \"", a64_cg_out);
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (c == '"' || c == '\\') {
            fprintf(a64_cg_out, "\\%c", c);
        } else if (c >= 32 && c < 127) {
            fputc(c, a64_cg_out);
        } else {
            fprintf(a64_cg_out, "\\%03o", c);
        }
    }
    fputs("\"\n", a64_cg_out);
}

static void a64_cg_data_zero(size_t size) {
    if (size) {
        a64_emit(".space %lu", (unsigned long)size);
    }
}

static void a64_cg_string(int label, const char *bytes, size_t len) {
    a64_emit(".section __TEXT,__const");
    peep_flush();
    fprintf(a64_cg_out, "L.str.%d:\n", label);
    a64_cg_data_bytes(bytes, len);
}

/* the caller's outgoing area starts right above the saved x29 and x30 */
static long a64_cg_stack_param_offset(size_t offset) {
    return 16 + (long)offset;
}

/* ------------------------------------------------------------------ */
/* the calling convention                                             */
/* ------------------------------------------------------------------ */

/*
 * AAPCS64 with Apple's variations: named arguments go in x0-x7, every
 * variadic argument goes on the stack. A record of up to 16 bytes is
 * passed in one or two consecutive registers when enough are left,
 * otherwise on the stack and no later argument uses a register. A larger
 * record is copied and passed by reference like a pointer. Records of
 * up to 16 bytes are returned in x0 and x1, larger ones through x8.
 * On the stack, Apple packs a named scalar at its natural size and
 * alignment (a 9th `char` takes one byte); a record takes a multiple of
 * 8 bytes at 8 byte alignment and a variadic argument always takes 8,
 * so the variadic part starts on an 8 byte boundary.
 */
static void a64_cg_args_begin(ArgAlloc *a, size_t nfixed, int sret) {
    UNUSED(sret);       /* x8 is not an argument register */
    a->nregs = 0;
    a->nfregs = 0;
    a->nstack = 0;
    a->nfixed = nfixed;
    a->index = 0;
}

static void a64_stack_bytes(ArgAlloc *a, size_t size, size_t align, ArgLoc *loc) {
    a->nstack = a64_round_up(a->nstack, align);
    loc->in_regs = 0;
    loc->offset = a->nstack;
    loc->size = size;
    a->nstack += size;
}

static void a64_cg_args_next(ArgAlloc *a, const ArgType *t, ArgLoc *loc) {
    ArgKind kind = t->kind;
    size_t size = t->size;
    size_t align = t->align;
    int is_record = kind == ARG_RECORD || kind == ARG_RECORD_MEMORY;    /* packing does not matter to AAPCS */
    size_t n = is_record || size == 16 ? (size + 7) / 8 : 1;
    int variadic = a->index++ >= a->nfixed;

    loc->by_ref = 0;
    loc->in_fregs = 0;
    loc->reg = loc->freg = loc->nregs = loc->offset = loc->size = loc->copy = loc->copy_size = 0;
    loc->type = *t;
    loc->type.sse = 0;      /* eightbyte classes are x86_64's; here only an HFA uses vector registers */
    if (variadic) {
        size = 8 * n;
        align = align > 8 ? align : 8;
    }
    if (kind == ARG_FLOAT) {
        if (!variadic && a->nfregs < 8) {
            loc->in_regs = 1;
            loc->in_fregs = 1;
            loc->reg = loc->freg = a->nfregs++;
            loc->nregs = 1;
            return;
        }
        a64_stack_bytes(a, size, align, loc);
        return;
    }
    if (is_record && t->hfa && !variadic) {
        /*
         * A homogeneous floating point aggregate: one vector register per
         * member, or all on the stack, where Apple packs it like a scalar
         * (at its member's alignment, its exact size) rather than in
         * eightbytes like other records.
         */
        if (a->nfregs + t->hfa <= 8) {
            loc->in_regs = 1;
            loc->in_fregs = 1;
            loc->reg = loc->freg = a->nfregs;
            loc->nregs = t->hfa;
            a->nfregs += t->hfa;
            return;
        }
        a->nfregs = 8;
        a64_stack_bytes(a, t->hfa * t->hfa_size, t->hfa_size, loc);
        return;
    }
    if (kind == ARG_INT && size == 16) {
        /* an __int128 takes the next two registers (Apple does not round up to an even pair as AAPCS does), or 16 aligned bytes of the stack */
        if (!variadic && a->nregs + 2 <= 8) {
            loc->in_regs = 1;
            loc->reg = a->nregs;
            loc->nregs = 2;
            a->nregs += 2;
            return;
        }
        a->nregs = 8;
        a64_stack_bytes(a, 16, 16, loc);
        return;
    }
    if (is_record && size > 16) {
        loc->by_ref = 1;
        loc->copy_size = 8 * n;
        n = 1;
        size = 8;
        align = 8;
    } else if (is_record) {
        size = 8 * n;
        align = align > 8 ? align : 8;
    }
    if (!variadic && a->nregs + n <= 8) {
        loc->in_regs = 1;
        loc->reg = a->nregs;
        loc->nregs = n;
        a->nregs += n;
        return;
    }
    if (!variadic && n > 1) {
        a->nregs = 8;
    }
    a64_stack_bytes(a, size, align, loc);
}

static size_t a64_cg_args_end(ArgAlloc *a, ArgLoc *locs, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (locs[i].by_ref) {
            a->nstack = a64_round_up(a->nstack, 8);
            locs[i].copy = a->nstack;
            a->nstack += locs[i].copy_size;
        }
    }
    return a->nstack;
}

static int a64_cg_record_ret_regs(const ArgType *t) {
    return t->hfa || t->size <= 16;
}

/* ------------------------------------------------------------------ */
/* instruction selection                                              */
/* ------------------------------------------------------------------ */

static IRFunc *a64_F = NULL;
static int a64_label_base = 0;      /* block b's label is L<a64_label_base + b> */
static int *a64_fold_of = NULL;     /* per instruction: the address computation folded into this memory access, or -1 */
static int *a64_folded = NULL;      /* per instruction: 1 when it was folded into its user and is not emitted */
static int *a64_combine = NULL;     /* per instruction, at -O1: how it absorbs the instruction before it (A64_C_*) */
static int *a64_block_start = NULL; /* per instruction: the first of its block */

#define A64_C_UBFX 1        /* and (lshr x, s), 2^k - 1: ubfx */
#define A64_C_FIELD 2       /* sext or zext 4 of and x, 2^k - 1 (k < 32), itself perhaps a ubfx: one 64 bit and or ubfx */
#define A64_C_SHIFT_A 3     /* add (shl x, k), y: add y, x, lsl #k */
#define A64_C_SHIFT_B 4     /* add y, (shl x, k) */
#define A64_C_MADD_A 5      /* add (mul x, z), y: madd */
#define A64_C_MADD_B 6      /* add y, (mul x, z) */
#define A64_C_FMADD_A 7     /* fadd (fmul x, z), y: fmadd (C allows contracting the two, as clang does) */
#define A64_C_FMADD_B 8     /* fadd y, (fmul x, z) */
#define A64_C_FMSUB 9       /* fsub y, (fmul x, z): fmsub, y - x * z */
#define A64_C_FNMSUB 10     /* fsub (fmul x, z), y: fnmsub, x * z - y */
static int *a64_npreds = NULL;      /* per block */

/* the allocatable registers in the allocator's three tiers: arguments, other caller-saved, callee-saved */
static const int a64_iregs[] = { 0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 14, 15, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28 };
static const int a64_fregs[] = { 0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 8, 9, 10, 11, 12, 13, 14, 15 };

/* the floating point scratch registers */
#define FS0 28
#define FS1 29
#define FS2 30

static int a64_cg_clobbers(const IRFunc *f, const IRInst *i);

static const RegTarget a64_regtarget = {
    { a64_iregs, 8, 12, 22, 0 },
    { a64_fregs, 8, 20, 28, 0 },
    a64_cg_clobbers
};

/* what an instruction's code writes besides its destination: the runtime calls everything caller-saved, the __int128 pairs x1-x4, va_arg and va_start x1, a record return x0 and x1 */
static int a64_cg_clobbers(const IRFunc *f, const IRInst *i) {
    if (ir_clobbers_registers(f, i)) {
        return RA_CLOBBER_CALL;
    }
    if (regalloc_has_wide(f, i) || i->op == IR_VAARG || i->op == IR_VASTART || (i->op == IR_RET && (i->flags & IRF_RECORD))) {
        return RA_CLOBBER_ARGS;
    }
    return RA_CLOBBER_NONE;
}

static IRType a64_vtype(int v) {
    return (IRType)a64_F->vregs[v].type;
}

/* the machine register holding `v`, or -1 when it has a frame slot */
static int a64_loc_reg(int v) {
    const IRVReg *r = &a64_F->vregs[v];
    if (r->reg < 0) {
        return -1;
    }
    return ir_type_is_float((IRType)r->type) ? a64_fregs[r->reg] : a64_iregs[r->reg];
}

/* x<scratch> (or its own register) = v */
static int a64_use_x(int v, int scratch) {
    int r = a64_loc_reg(v);
    char buf[32];
    if (r >= 0) {
        return r;
    }
    if (a64_F->vregs[v].offset) {
        a64_emit("ldr x%d, [%s]", scratch, frame_operand(a64_F->vregs[v].offset, 8, buf, sizeof(buf)));
    }
    return scratch;
}

static int use_d(int v, int scratch) {
    int r = a64_loc_reg(v);
    char buf[32];
    if (r >= 0) {
        return r;
    }
    if (a64_F->vregs[v].offset) {
        a64_emit("ldr d%d, [%s]", scratch, frame_operand(a64_F->vregs[v].offset, 8, buf, sizeof(buf)));
    }
    return scratch;
}

/* the register to compute `v` into */
static int a64_def_reg(int v, int scratch) {
    int r = a64_loc_reg(v);
    return r >= 0 ? r : scratch;
}

/* `v` has been computed into x<r>: move it to its register, or store it to its slot */
static void a64_def_done_x(int v, int r) {
    char buf[32];
    int p = a64_loc_reg(v);
    if (p >= 0) {
        if (p != r) {
            a64_emit("mov x%d, x%d", p, r);
        }
    } else if (a64_F->vregs[v].offset) {
        a64_emit("str x%d, [%s]", r, frame_operand(a64_F->vregs[v].offset, 8, buf, sizeof(buf)));
    }
}

static void def_done_d(int v, int r) {
    char buf[32];
    int p = a64_loc_reg(v);
    if (p >= 0) {
        if (p != r) {
            a64_emit("fmov d%d, d%d", p, r);
        }
    } else if (a64_F->vregs[v].offset) {
        a64_emit("str d%d, [%s]", r, frame_operand(a64_F->vregs[v].offset, 8, buf, sizeof(buf)));
    }
}

/* an __int128 lives in a 16 byte slot: x<lo>:x<hi> = v, and back */
static void a64_use_pair(int v, int lo, int hi) {
    long off = a64_F->vregs[v].offset;
    if (!off) {
        return;
    }
    if (pair_off_encodable(off)) {
        a64_emit("ldp x%d, x%d, [x29, #%ld]", lo, hi, off);
    } else {
        fp_addr("x16", off);
        a64_emit("ldp x%d, x%d, [x16]", lo, hi);
    }
}

static void a64_def_pair_done(int v, int lo, int hi) {
    long off = a64_F->vregs[v].offset;
    if (!off) {
        return;
    }
    if (pair_off_encodable(off)) {
        a64_emit("stp x%d, x%d, [x29, #%ld]", lo, hi, off);
    } else {
        fp_addr("x16", off);
        a64_emit("stp x%d, x%d, [x16]", lo, hi);
    }
}

static const char *a64_blabel(int b) {
    static char bufs[4][16];
    static int next = 0;
    char *buf = bufs[next++ & 3];
    snprintf(buf, 16, "L%d", a64_label_base + b);
    return buf;
}

static const char *icond(IRCond cc) {
    switch (cc) {
        case IR_EQ: return "eq";
        case IR_NE: return "ne";
        case IR_LT: return "lt";
        case IR_GE: return "ge";
        case IR_GT: return "gt";
        case IR_LE: return "le";
        case IR_ULT: return "lo";
        case IR_UGE: return "hs";
        case IR_UGT: return "hi";
        case IR_ULE: return "ls";
    }
    return "ne";
}

/* an unordered comparison sets C and V only, so each of these is false for a NaN except ne */
static const char *fcond(IRCond cc) {
    switch (cc) {
        case IR_EQ: return "eq";
        case IR_NE: return "ne";
        case IR_LT: return "mi";
        case IR_LE: return "ls";
        case IR_GT: return "gt";
        case IR_GE: return "ge";
        default: return "ne";
    }
}

static const char *invert_cond(const char *c) {
    static const char *const pairs[] = { "eq", "ne", "lt", "ge", "gt", "le", "lo", "hs", "hi", "ls", "mi", "pl", NULL };
    int i;
    for (i = 0; pairs[i]; i++) {
        if (strcmp(pairs[i], c) == 0) {
            return pairs[i ^ 1];
        }
    }
    return NULL;
}

/* the memory access size of a load, store or bit-field instruction */
static int a64_access_size(const IRInst *i) {
    if (i->op == IR_LOAD || i->op == IR_STORE || i->op == IR_LOADBITS || i->op == IR_STOREBITS) {
        return i->size ? i->size : (int)ir_type_size((IRType)i->type);
    }
    return 8;
}

/*
 * Addressing modes. An addr_local whose only use is the address of a
 * load or store, or an add of a constant to a pointer whose only use is
 * the access right after it, is folded into that access as [x29, #off]
 * or [xN, #off]; the folded temporary gets no place (this runs before
 * the allocator).
 */
/* the instruction before i in its block computes v for i alone, so i can absorb it */
static int a64_feeds(int i, int v) {
    const IRInst *d;
    const IRVReg *r;
    if (i == 0 || v < 0 || a64_block_start[i]) {
        return 0;
    }
    d = &a64_F->insts[i - 1];
    r = &a64_F->vregs[v];
    return d->dst == v && !r->is_local && r->nuses == 1 && r->def == i - 1 && !a64_folded[i - 1];
}

/* the k of a mask 2^k - 1 of an integer type, 0 if it is none */
static int a64_mask_bits(unsigned long m, IRType t) {
    int k = 0;
    if (t == IR_I32) {
        m &= 0xffffffffUL;
    }
    while (m & 1) {
        m >>= 1;
        k++;
    }
    return m == 0 ? k : 0;
}

/*
 * At -O1, instructions that one arm64 instruction does together:
 * a shift and a mask are a bit-field extract, the extension of a small
 * field is free in a 64 bit extract, and an add takes its other operand
 * shifted. The absorbed instruction is right before its only user, so
 * its operands are still in place there, and it gets no place itself.
 */
static void a64_find_combines(void) {
    int i;
    for (i = 1; i < a64_F->ninsts; i++) {
        const IRInst *u = &a64_F->insts[i];
        const IRInst *d = &a64_F->insts[i - 1];
        IRType t = (IRType)u->type;
        int bits = t == IR_I32 ? 32 : 64;
        int kind = 0;
        if (u->op == IR_AND && (u->flags & IRF_IMM) && (t == IR_I32 || t == IR_I64) && a64_feeds(i, u->a)
                && d->op == IR_LSHR && (d->flags & IRF_IMM) && d->type == t) {
            int k = a64_mask_bits((unsigned long)u->imm, t);
            int sh = (int)(d->imm & (bits - 1));
            if (k > 0 && sh + k <= bits) {
                kind = A64_C_UBFX;
            }
        } else if ((u->op == IR_SEXT || u->op == IR_ZEXT) && u->size == 4 && t == IR_I64 && a64_feeds(i, u->a)
                && d->op == IR_AND && (d->flags & IRF_IMM) && d->type == IR_I32) {
            int k = a64_mask_bits((unsigned long)d->imm, IR_I32);
            if (k > 0 && k < 32) {
                kind = A64_C_FIELD;
            }
        } else if (u->op == IR_ADD && !(u->flags & IRF_IMM) && t == IR_I64 && u->a != u->b && d->op == IR_SHL
                && (d->flags & IRF_IMM) && d->type == IR_I64) {
            if (a64_feeds(i, u->a)) {
                kind = A64_C_SHIFT_A;
            } else if (a64_feeds(i, u->b)) {
                kind = A64_C_SHIFT_B;
            }
        } else if ((u->op == IR_FADD || u->op == IR_FSUB) && (t == IR_F32 || t == IR_F64) && u->a != u->b && d->op == IR_FMUL
                && d->type == t) {
            if (a64_feeds(i, u->a)) {
                kind = u->op == IR_FADD ? A64_C_FMADD_A : A64_C_FNMSUB;
            } else if (a64_feeds(i, u->b)) {
                kind = u->op == IR_FADD ? A64_C_FMADD_B : A64_C_FMSUB;
            }
        } else if (u->op == IR_ADD && !(u->flags & IRF_IMM) && (t == IR_I32 || t == IR_I64) && u->a != u->b && d->op == IR_MUL
                && d->type == t) {
            if (a64_feeds(i, u->a)) {
                kind = A64_C_MADD_A;
            } else if (a64_feeds(i, u->b)) {
                kind = A64_C_MADD_B;
            }
        }
        if (kind) {
            a64_combine[i] = kind;
            a64_folded[i - 1] = 1;
            a64_F->vregs[d->dst].folded = 1;
        }
    }
}

/* a constant for an operation of type t in x10: an i32's upper half is never read, so at -O1 it may be the sign's, which a negative loads in fewer instructions */
static unsigned long a64_imm32(IRType t, long v) {
    if (t != IR_I32) {
        return (unsigned long)v;
    }
    return a64_F->opt_level > 0 ? (unsigned long)(long)(int)v : (unsigned long)(unsigned)v;
}

/* a constant or address an integer register argument is made of: computed straight into the argument register */
static int a64_arg_foldable(int v) {
    const IRVReg *r = &a64_F->vregs[v];
    const IRInst *d;
    if (r->is_local || r->nuses != 1 || r->def < 0 || a64_folded[r->def] || (r->type != IR_I32 && r->type != IR_I64)) {
        return 0;
    }
    d = &a64_F->insts[r->def];
    return d->op == IR_ICONST || d->op == IR_ADDR_GLOBAL || d->op == IR_ADDR_STRING;
}

static void a64_find_arg_folds(void) {
    int i, k;
    for (i = 0; i < a64_F->ninsts; i++) {
        const IRInst *ins = &a64_F->insts[i];
        const IRCall *c;
        ArgAlloc alloc;
        ArgLoc loc;
        if (ins->op != IR_CALL) {
            continue;
        }
        c = &a64_F->calls[ins->imm];
        a64_cg_args_begin(&alloc, c->nfixed, c->sret_addr >= 0);
        for (k = 0; k < c->nargs; k++) {
            const IRArg *a = &a64_F->args[c->first_arg + k];
            a64_cg_args_next(&alloc, &a->type, &loc);
            if (loc.in_regs && !loc.in_fregs && !loc.by_ref && !a->by_addr && a->type.size <= 8 && a64_arg_foldable(a->v)) {
                a64_folded[a64_F->vregs[a->v].def] = 1;
                a64_F->vregs[a->v].folded = 1;
            }
        }
    }
}

/* x<n> = the constant or address that folded argument v is */
static void a64_arg_value(int n, int v) {
    const IRInst *d = &a64_F->insts[a64_F->vregs[v].def];
    char reg[8];
    snprintf(reg, sizeof(reg), "x%d", n);
    if (d->op == IR_ICONST) {
        load_imm(reg, a64_imm32((IRType)d->type, d->imm));
    } else if (d->op == IR_ADDR_GLOBAL) {
        global_addr(reg, d->sym, d->flags & IRF_EXTERN);
    } else {
        a64_emit("adrp %s, L.str.%ld@PAGE", reg, d->imm);
        a64_emit("add %s, %s, L.str.%ld@PAGEOFF", reg, reg, d->imm);
    }
}

static void a64_find_folds(void) {
    int i, b;
    for (i = 0; i < a64_F->ninsts; i++) {
        a64_fold_of[i] = -1;
        a64_folded[i] = 0;
        a64_combine[i] = 0;
        a64_block_start[i] = 0;
    }
    for (b = 0; b < a64_F->nblocks; b++) {
        if (a64_F->blocks[b].start >= 0 && a64_F->blocks[b].start < a64_F->ninsts) {
            a64_block_start[a64_F->blocks[b].start] = 1;
        }
    }
    for (i = 0; i < a64_F->ninsts; i++) {
        const IRInst *d = &a64_F->insts[i];
        const IRVReg *v;
        const IRInst *u;
        int size;
        if (d->dst < 0 || (d->op != IR_ADDR_LOCAL && !(d->op == IR_ADD && (d->flags & IRF_IMM) && d->type == IR_I64))) {
            continue;
        }
        v = &a64_F->vregs[d->dst];
        if (v->is_local || v->nuses != 1 || v->def != i || v->use <= i) {
            continue;
        }
        u = &a64_F->insts[v->use];
        if (u->op != IR_LOAD && u->op != IR_STORE && u->op != IR_LOADBITS && u->op != IR_STOREBITS) {
            continue;
        }
        if (u->a != d->dst || u->b == d->dst) {
            continue;
        }
        /* an add's pointer operand is read at the access instead, so nothing may come between */
        if (d->op == IR_ADD && v->use != i + 1) {
            continue;
        }
        size = a64_access_size(u);
        if (size == 16 ? !pair_off_encodable(d->imm) : !off_encodable(d->imm, size)) {
            continue;
        }
        a64_fold_of[v->use] = i;
        a64_folded[i] = 1;
        a64_F->vregs[d->dst].folded = 1;
    }
    if (a64_F->opt_level > 0) {
        a64_find_combines();
        a64_find_arg_folds();
    }
}

/* the address operand of memory instruction `i`: "x29, #off", "xN, #off" or "xN"; the base is loaded into x<scratch> if needed */
static const char *a64_mem_operand(int i, int scratch, char *buf, size_t n) {
    const IRInst *ins = &a64_F->insts[i];
    if (a64_fold_of[i] >= 0) {
        const IRInst *d = &a64_F->insts[a64_fold_of[i]];
        if (d->op == IR_ADDR_LOCAL) {
            snprintf(buf, n, "x29, #%ld", d->imm);
        } else {
            int base = a64_use_x(d->a, scratch);
            if (d->imm) {
                snprintf(buf, n, "x%d, #%ld", base, d->imm);
            } else {
                snprintf(buf, n, "x%d", base);
            }
        }
        return buf;
    }
    snprintf(buf, n, "x%d", a64_use_x(ins->a, scratch));
    return buf;
}

/* x<n> = the `len` (1..8) bytes at [x<base> + off], zero extended; x17 is scratch for the pieces */
static void a64_load_word(int n, int base, size_t off, size_t len) {
    size_t pos = 0;
    while (pos < len) {
        size_t chunk = len - pos >= 8 ? 8 : len - pos >= 4 ? 4 : len - pos >= 2 ? 2 : 1;
        int dst = pos == 0 ? n : 17;
        switch (chunk) {
            case 8: a64_emit("ldr x%d, [x%d, #%lu]", dst, base, (unsigned long)(off + pos)); break;
            case 4: a64_emit("ldr w%d, [x%d, #%lu]", dst, base, (unsigned long)(off + pos)); break;
            case 2: a64_emit("ldrh w%d, [x%d, #%lu]", dst, base, (unsigned long)(off + pos)); break;
            default: a64_emit("ldrb w%d, [x%d, #%lu]", dst, base, (unsigned long)(off + pos)); break;
        }
        if (pos) {
            a64_emit("orr x%d, x%d, x17, lsl #%lu", n, n, (unsigned long)(8 * pos));
        }
        pos += chunk;
    }
}

/* copy `size` bytes from [x<src>] to [x<dst>]; x16 and x17 are scratch, and x1 counts a long copy */
static void a64_copy_bytes(int dst, int src, size_t size) {
    size_t off = 0;
    if (size <= 64) {
        while (size - off >= 8) {
            a64_emit("ldr x16, [x%d, #%lu]", src, (unsigned long)off);
            a64_emit("str x16, [x%d, #%lu]", dst, (unsigned long)off);
            off += 8;
        }
        if (size - off >= 4) {
            a64_emit("ldr w16, [x%d, #%lu]", src, (unsigned long)off);
            a64_emit("str w16, [x%d, #%lu]", dst, (unsigned long)off);
            off += 4;
        }
        if (size - off >= 2) {
            a64_emit("ldrh w16, [x%d, #%lu]", src, (unsigned long)off);
            a64_emit("strh w16, [x%d, #%lu]", dst, (unsigned long)off);
            off += 2;
        }
        if (size - off >= 1) {
            a64_emit("ldrb w16, [x%d, #%lu]", src, (unsigned long)off);
            a64_emit("strb w16, [x%d, #%lu]", dst, (unsigned long)off);
        }
    } else {
        int l = ++a64_nlabels;
        load_imm("x17", size);
        peep_flush();
        fprintf(a64_cg_out, "Lcg%d:\n", l);
        a64_emit("sub x17, x17, #1");
        a64_emit("ldrb w16, [x%d, x17]", src);
        a64_emit("strb w16, [x%d, x17]", dst);
        a64_emit("cbnz x17, Lcg%d", l);
    }
}

static void a64_zero_bytes(int dst, size_t size) {
    size_t off = 0;
    if (size <= 64) {
        while (size - off >= 8) {
            a64_emit("str xzr, [x%d, #%lu]", dst, (unsigned long)off);
            off += 8;
        }
        if (size - off >= 4) {
            a64_emit("str wzr, [x%d, #%lu]", dst, (unsigned long)off);
            off += 4;
        }
        if (size - off >= 2) {
            a64_emit("strh wzr, [x%d, #%lu]", dst, (unsigned long)off);
            off += 2;
        }
        if (size - off >= 1) {
            a64_emit("strb wzr, [x%d, #%lu]", dst, (unsigned long)off);
        }
    } else {
        int l = ++a64_nlabels;
        load_imm("x17", size);
        peep_flush();
        fprintf(a64_cg_out, "Lcg%d:\n", l);
        a64_emit("sub x17, x17, #1");
        a64_emit("strb wzr, [x%d, x17]", dst);
        a64_emit("cbnz x17, Lcg%d", l);
    }
}

/* save (or restore) the callee-saved registers the allocator used, in pairs; a far save area is addressed through x16 */
static void a64_saved_regs(int restore) {
    long span = 8 * (a64_F->nsaved_ints + a64_F->nsaved_floats);
    long off = a64_F->saved_off;
    const char *base = "x29";
    int k = 0;
    if (span == 0) {
        return;
    }
    if (off < -256 || off + span > 256) {
        fp_addr("x16", off);
        base = "x16";
        off = 0;
    }
    while (k + 1 < a64_F->nsaved_ints) {
        a64_emit("%s x%d, x%d, [%s, #%ld]", restore ? "ldp" : "stp", a64_iregs[a64_F->saved_ints[k]], a64_iregs[a64_F->saved_ints[k + 1]], base, off);
        off += 16;
        k += 2;
    }
    if (k < a64_F->nsaved_ints) {
        a64_emit("%s x%d, [%s, #%ld]", restore ? "ldr" : "str", a64_iregs[a64_F->saved_ints[k]], base, off);
        off += 8;
    }
    for (k = 0; k < a64_F->nsaved_floats; k++) {
        a64_emit("%s d%d, [%s, #%ld]", restore ? "ldr" : "str", a64_fregs[a64_F->saved_floats[k]], base, off);
        off += 8;
    }
}

static int a64_leaf = 0;    /* the function has no frame record (-O1, a leaf with nothing in memory) */

/* the epilogue, at every return */
static void a64_epilogue(void) {
    if (a64_leaf) {
        a64_emit("ret");
        return;
    }
    a64_saved_regs(1);
    a64_emit("mov sp, x29");
    a64_emit("ldp x29, x30, [sp], #16");
    a64_emit("ret");
}

/*
 * At -O1 a function that calls nothing and keeps nothing in its frame
 * (no slots, no saved registers, every parameter in a register) needs
 * no frame record: x29 and x30 are left alone, as clang does for leaves.
 */
static int a64_is_leaf(void) {
    int i;
    if (a64_F->opt_level <= 0 || a64_F->frame_size || a64_F->nsaved_ints || a64_F->nsaved_floats || a64_F->is_variadic
            || a64_F->sret_off) {
        return 0;
    }
    for (i = 0; i < a64_F->nparams; i++) {
        const IRParam *p = &a64_F->params[i];
        if (!p->loc.in_regs || p->vreg < 0 || p->kr_from || p->by_addr || p->wide) {
            return 0;
        }
    }
    for (i = 0; i < a64_F->ninsts; i++) {
        const IRInst *ins = &a64_F->insts[i];
        if (ir_clobbers_registers(a64_F, ins) || ins->op == IR_ADDR_LOCAL || ins->op == IR_ALLOCA || ins->op == IR_VASTART
                || ins->op == IR_VAARG) {
            return 0;
        }
    }
    return 1;
}

/* a + b, a - b and the like with a constant: the immediate forms when they encode, else through x10 */
static void binop_imm(IROp op, IRType t, int d, int a, long imm) {
    const char *rd = rn(d, t);
    const char *ra = rn(a, t);
    unsigned long u = (unsigned long)imm;
    int bits = t == IR_I32 ? 32 : 64;
    switch (op) {
        case IR_ADD:
        case IR_SUB: {
            long v = op == IR_SUB ? (long)(0 - u) : imm;
            if (v == 0) {
                if (d != a) {
                    a64_emit("mov %s, %s", rd, ra);
                }
                return;
            }
            if (v > 0 && v <= 4095) {
                a64_emit("add %s, %s, #%ld", rd, ra, v);
                return;
            }
            if (v < 0 && v >= -4095) {
                a64_emit("sub %s, %s, #%ld", rd, ra, -v);
                return;
            }
            break;
        }
        case IR_MUL:
            if (u != 0 && (u & (u - 1)) == 0) {
                if (u == 1) {
                    if (d != a) {
                        a64_emit("mov %s, %s", rd, ra);
                    }
                } else {
                    a64_emit("lsl %s, %s, #%d", rd, ra, a64_log2_of(u));
                }
                return;
            }
            break;
        case IR_SHL:
        case IR_LSHR:
        case IR_ASHR:
            imm &= bits - 1;
            if (imm == 0) {
                if (d != a) {
                    a64_emit("mov %s, %s", rd, ra);
                }
            } else {
                a64_emit("%s %s, %s, #%ld", op == IR_SHL ? "lsl" : op == IR_LSHR ? "lsr" : "asr", rd, ra, imm);
            }
            return;
        case IR_AND:
        case IR_OR:
        case IR_XOR:
            if (t == IR_I32 ? is_logical_imm32(u) : is_logical_imm(u)) {
                a64_emit("%s %s, %s, #0x%lx", op == IR_AND ? "and" : op == IR_OR ? "orr" : "eor", rd, ra,
                     t == IR_I32 ? u & 0xffffffffUL : u);
                return;
            }
            break;
        default:
            break;
    }
    load_imm("x10", a64_imm32(t, imm));
    switch (op) {
        case IR_ADD: a64_emit("add %s, %s, %s", rd, ra, rn(10, t)); break;
        case IR_SUB: a64_emit("sub %s, %s, %s", rd, ra, rn(10, t)); break;
        case IR_MUL: a64_emit("mul %s, %s, %s", rd, ra, rn(10, t)); break;
        case IR_SDIV: a64_emit("sdiv %s, %s, %s", rd, ra, rn(10, t)); break;
        case IR_UDIV: a64_emit("udiv %s, %s, %s", rd, ra, rn(10, t)); break;
        case IR_SREM:
            a64_emit("sdiv %s, %s, %s", rn(16, t), ra, rn(10, t));
            a64_emit("msub %s, %s, %s, %s", rd, rn(16, t), rn(10, t), ra);
            break;
        case IR_UREM:
            a64_emit("udiv %s, %s, %s", rn(16, t), ra, rn(10, t));
            a64_emit("msub %s, %s, %s, %s", rd, rn(16, t), rn(10, t), ra);
            break;
        case IR_AND: a64_emit("and %s, %s, %s", rd, ra, rn(10, t)); break;
        case IR_OR: a64_emit("orr %s, %s, %s", rd, ra, rn(10, t)); break;
        default: a64_emit("eor %s, %s, %s", rd, ra, rn(10, t)); break;
    }
}

static void binop_reg(IROp op, IRType t, int d, int a, int b) {
    const char *rd = rn(d, t);
    const char *ra = rn(a, t);
    const char *rb = rn(b, t);
    switch (op) {
        case IR_ADD: a64_emit("add %s, %s, %s", rd, ra, rb); break;
        case IR_SUB: a64_emit("sub %s, %s, %s", rd, ra, rb); break;
        case IR_MUL: a64_emit("mul %s, %s, %s", rd, ra, rb); break;
        case IR_SDIV: a64_emit("sdiv %s, %s, %s", rd, ra, rb); break;
        case IR_UDIV: a64_emit("udiv %s, %s, %s", rd, ra, rb); break;
        case IR_SREM:
            a64_emit("sdiv %s, %s, %s", rn(16, t), ra, rb);
            a64_emit("msub %s, %s, %s, %s", rd, rn(16, t), rb, ra);
            break;
        case IR_UREM:
            a64_emit("udiv %s, %s, %s", rn(16, t), ra, rb);
            a64_emit("msub %s, %s, %s, %s", rd, rn(16, t), rb, ra);
            break;
        case IR_SHL: a64_emit("lsl %s, %s, %s", rd, ra, rb); break;
        case IR_LSHR: a64_emit("lsr %s, %s, %s", rd, ra, rb); break;
        case IR_ASHR: a64_emit("asr %s, %s, %s", rd, ra, rb); break;
        case IR_AND: a64_emit("and %s, %s, %s", rd, ra, rb); break;
        case IR_OR: a64_emit("orr %s, %s, %s", rd, ra, rb); break;
        default: a64_emit("eor %s, %s, %s", rd, ra, rb); break;
    }
}

/*
 * __int128 arithmetic on register pairs: a in x9:x10, b in x1:x2, the
 * result in x3:x4 (shifts work in place on a). The compiler runtime
 * (libcompiler_rt, in libSystem) divides and converts to and from
 * floating point.
 */
static void a64_wide_shift(IROp op) {
    int l_big = ++a64_nlabels;
    int l_done = ++a64_nlabels;
    /* the count is in x1; a register shift takes it modulo 64, so the two ranges are separate code */
    a64_emit("cmp x1, #64");
    a64_emit("b.hs Lcg%d", l_big);
    a64_emit("mov x17, #63");
    a64_emit("sub x17, x17, x1");           /* 63 - count: the other half shifted by 1, then by this, never by 64 */
    if (op == IR_SHL) {
        a64_emit("lsl x10, x10, x1");
        a64_emit("lsr x16, x9, #1");
        a64_emit("lsr x16, x16, x17");
        a64_emit("orr x10, x10, x16");
        a64_emit("lsl x9, x9, x1");
    } else {
        a64_emit("lsr x9, x9, x1");
        a64_emit("lsl x16, x10, #1");
        a64_emit("lsl x16, x16, x17");
        a64_emit("orr x9, x9, x16");
        a64_emit(op == IR_LSHR ? "lsr x10, x10, x1" : "asr x10, x10, x1");
    }
    a64_emit("b Lcg%d", l_done);
    peep_flush();
    fprintf(a64_cg_out, "Lcg%d:\n", l_big);
    a64_emit("sub x16, x1, #64");
    if (op == IR_SHL) {
        a64_emit("lsl x10, x9, x16");
        a64_emit("mov x9, #0");
    } else if (op == IR_LSHR) {
        a64_emit("lsr x9, x10, x16");
        a64_emit("mov x10, #0");
    } else {
        a64_emit("asr x9, x10, x16");
        a64_emit("asr x10, x10, #63");
    }
    peep_flush();
    fprintf(a64_cg_out, "Lcg%d:\n", l_done);
}

/* set the flags for an __int128 comparison of x9:x10 with x1:x2; returns the condition */
static const char *a64_wide_compare(IRCond cc) {
    switch (cc) {
        case IR_EQ:
        case IR_NE:
            a64_emit("eor x16, x9, x1");
            a64_emit("eor x17, x10, x2");
            a64_emit("orr x16, x16, x17");
            a64_emit("cmp x16, #0");
            return icond(cc);
        /* the flags of the 128 bit subtraction: lt and lo (a borrow) are a < b, the others are the swap or the negation */
        case IR_GT:
        case IR_LE:
        case IR_UGT:
        case IR_ULE:
            a64_emit("cmp x1, x9");
            a64_emit("sbcs xzr, x2, x10");
            return cc == IR_GT ? "lt" : cc == IR_LE ? "ge" : cc == IR_UGT ? "lo" : "hs";
        default:
            a64_emit("cmp x9, x1");
            a64_emit("sbcs xzr, x10, x2");
            return cc == IR_LT ? "lt" : cc == IR_GE ? "ge" : cc == IR_ULT ? "lo" : "hs";
    }
}

static void a64_select_wide(const IRInst *i) {
    IROp op = (IROp)i->op;
    switch (op) {
        case IR_ICONST:
            load_imm("x3", (unsigned long)i->imm);
            a64_emit(i->imm < 0 ? "mov x4, #-1" : "mov x4, #0");
            a64_def_pair_done(i->dst, 3, 4);
            return;
        case IR_MOV:
            a64_use_pair(i->a, 9, 10);
            a64_def_pair_done(i->dst, 9, 10);
            return;
        case IR_ADD:
        case IR_SUB:
        case IR_MUL:
        case IR_AND:
        case IR_OR:
        case IR_XOR:
            a64_use_pair(i->a, 9, 10);
            a64_use_pair(i->b, 1, 2);
            switch (op) {
                case IR_ADD: a64_emit("adds x3, x9, x1"); a64_emit("adc x4, x10, x2"); break;
                case IR_SUB: a64_emit("subs x3, x9, x1"); a64_emit("sbc x4, x10, x2"); break;
                case IR_MUL:
                    a64_emit("mul x16, x10, x1");           /* high: ahi*blo + alo*bhi + carry of alo*blo */
                    a64_emit("madd x16, x9, x2, x16");
                    a64_emit("umulh x17, x9, x1");
                    a64_emit("add x4, x16, x17");
                    a64_emit("mul x3, x9, x1");
                    break;
                case IR_AND: a64_emit("and x3, x9, x1"); a64_emit("and x4, x10, x2"); break;
                case IR_OR: a64_emit("orr x3, x9, x1"); a64_emit("orr x4, x10, x2"); break;
                default: a64_emit("eor x3, x9, x1"); a64_emit("eor x4, x10, x2"); break;
            }
            a64_def_pair_done(i->dst, 3, 4);
            return;
        case IR_SDIV:
        case IR_UDIV:
        case IR_SREM:
        case IR_UREM:
            a64_use_pair(i->a, 0, 1);
            a64_use_pair(i->b, 2, 3);
            a64_emit("bl _%s", op == IR_SDIV ? "__divti3" : op == IR_UDIV ? "__udivti3" : op == IR_SREM ? "__modti3" : "__umodti3");
            a64_def_pair_done(i->dst, 0, 1);
            return;
        case IR_SHL:
        case IR_LSHR:
        case IR_ASHR:
            a64_use_pair(i->a, 9, 10);
            {
                int c = a64_use_x(i->b, 1);
                if (c != 1) {
                    a64_emit("mov x1, x%d", c);
                }
            }
            a64_wide_shift(op);
            a64_def_pair_done(i->dst, 9, 10);
            return;
        case IR_NEG:
            a64_use_pair(i->a, 9, 10);
            a64_emit("negs x3, x9");
            a64_emit("ngc x4, x10");
            a64_def_pair_done(i->dst, 3, 4);
            return;
        case IR_NOT:
            a64_use_pair(i->a, 9, 10);
            a64_emit("mvn x3, x9");
            a64_emit("mvn x4, x10");
            a64_def_pair_done(i->dst, 3, 4);
            return;
        case IR_SEXT:
        case IR_ZEXT: {
            int a = a64_use_x(i->a, 9);
            if (i->size == 8) {
                if (a != 3) {
                    a64_emit("mov x3, x%d", a);
                }
            } else if (op == IR_SEXT) {
                a64_emit("sxtw x3, w%d", a);
            } else {
                a64_emit("mov w3, w%d", a);
            }
            if (op == IR_SEXT) {
                a64_emit("asr x4, x3, #63");
            } else {
                a64_emit("mov x4, #0");
            }
            a64_def_pair_done(i->dst, 3, 4);
            return;
        }
        case IR_LOAD: {
            char buf[32];
            const char *m = a64_mem_operand((int)(i - a64_F->insts), 9, buf, sizeof(buf));
            a64_emit("ldp x3, x4, [%s]", m);
            a64_def_pair_done(i->dst, 3, 4);
            return;
        }
        case IR_STORE: {
            char buf[32];
            const char *m;
            a64_use_pair(i->b, 9, 10);
            m = a64_mem_operand((int)(i - a64_F->insts), 1, buf, sizeof(buf));
            a64_emit("stp x9, x10, [%s]", m);
            return;
        }
        default:
            break;
    }
}

/* the arguments of a call: where they go, and the moves to get them there */
/*
 * At -O1 a call whose result the function returns at once, with every
 * argument in a register, is a tail call: the frame goes first and the
 * callee returns to this function's caller. Nothing of the frame may
 * still be needed, so the function must not have taken the address of
 * any of it.
 */
static int a64_tail_call(int index, const IRCall *c, size_t nstack) {
    const IRInst *ins = &a64_F->insts[index];
    const IRInst *r;
    int i;
    if (a64_F->opt_level <= 0 || index + 1 >= a64_F->ninsts || nstack || c->sret_addr >= 0 || c->result_addr >= 0
            || a64_F->is_variadic) {
        return 0;
    }
    r = &a64_F->insts[index + 1];
    if (r->op != IR_RET || (r->flags & IRF_RECORD) || (r->a >= 0 && r->a != ins->dst)) {
        return 0;
    }
    if (r->a >= 0 && a64_vtype(r->a) == IR_I128) {
        return 0;
    }
    for (i = 0; i < a64_F->ninsts; i++) {
        if (a64_F->insts[i].op == IR_ADDR_LOCAL || a64_F->insts[i].op == IR_ALLOCA || a64_F->insts[i].op == IR_VASTART) {
            return 0;
        }
    }
    return 1;
}

/* a call; returns 1 when it was made a tail call, which took the return after it along */
static int a64_select_call(int index) {
    const IRInst *ins = &a64_F->insts[index];
    const IRCall *c = &a64_F->calls[ins->imm];
    ArgLoc *locs = NULL;
    ArgAlloc alloc;
    size_t nstack;
    size_t area;
    int k, tail;
    size_t m;

    if (c->nargs) {
        locs = xmalloc((size_t)c->nargs * sizeof(*locs));
    }
    a64_cg_args_begin(&alloc, c->nfixed, c->sret_addr >= 0);
    for (k = 0; k < c->nargs; k++) {
        a64_cg_args_next(&alloc, &a64_F->args[c->first_arg + k].type, &locs[k]);
    }
    nstack = a64_cg_args_end(&alloc, locs, (size_t)c->nargs);
    tail = a64_tail_call(index, c, nstack);
    area = a64_round_up(nstack, 16);
    adjust_sp("sub", area);
    sp_disp = (long)area;

    /* what goes in memory: the copies of records passed by reference, and the stack arguments */
    for (k = 0; k < c->nargs; k++) {
        const IRArg *a = &a64_F->args[c->first_arg + k];
        const ArgLoc *l = &locs[k];
        char dst[32];
        if (l->by_ref) {
            int src = a64_use_x(a->v, 9);
            a64_emit("add x11, sp, #%lu", (unsigned long)l->copy);
            a64_copy_bytes(11, src, a->type.size);
            if (!l->in_regs) {
                a64_emit("str x11, [sp, #%lu]", (unsigned long)l->offset);
            }
        } else if (!l->in_regs) {
            if (a->by_addr) {
                int src = a64_use_x(a->v, 9);
                a64_emit("add x11, sp, #%lu", (unsigned long)l->offset);
                a64_copy_bytes(11, src, a->type.size);
            } else if (a->type.kind == ARG_FLOAT) {
                int r = use_d(a->v, FS0);
                a64_emit("str %s, [sp, #%lu]", a->type.size == 4 ? sn(r) : dn(r), (unsigned long)l->offset);
            } else if (a->type.size == 16) {
                a64_use_pair(a->v, 9, 10);
                a64_emit("stp x9, x10, [sp, #%lu]", (unsigned long)l->offset);
            } else {
                int r = a64_use_x(a->v, 9);
                snprintf(dst, sizeof(dst), "sp, #%lu", (unsigned long)l->offset);
                store_sized(r, a->type.size, dst);
            }
        }
    }
    /* what goes in registers; the sources are slots or registers outside the argument tier (src/regalloc.h) */
    for (k = 0; k < c->nargs; k++) {
        const IRArg *a = &a64_F->args[c->first_arg + k];
        const ArgLoc *l = &locs[k];
        if (!l->in_regs) {
            continue;
        }
        if (l->by_ref) {
            a64_emit("add x%lu, sp, #%lu", (unsigned long)l->reg, (unsigned long)l->copy);
        } else if (a->by_addr) {
            int src = a64_use_x(a->v, 16);
            if (l->type.hfa) {
                for (m = 0; m < l->type.hfa; m++) {
                    a64_emit("ldr %c%lu, [x%d, #%lu]", l->type.hfa_size == 4 ? 's' : 'd', (unsigned long)(l->freg + m), src,
                         (unsigned long)(m * l->type.hfa_size));
                }
            } else {
                for (m = 0; m < l->nregs; m++) {
                    size_t left = l->type.size - 8 * m;
                    a64_load_word((int)(l->reg + m), src, 8 * m, left >= 8 ? 8 : left);
                }
            }
        } else if (a->type.size == 16) {
            a64_use_pair(a->v, (int)l->reg, (int)l->reg + 1);
        } else if (l->in_fregs) {
            int r = use_d(a->v, (int)l->freg);
            if (r != (int)l->freg) {
                a64_emit("fmov d%lu, d%d", (unsigned long)l->freg, r);
            }
        } else if (a64_F->vregs[a->v].folded) {
            a64_arg_value((int)l->reg, a->v);
        } else {
            int r = a64_use_x(a->v, (int)l->reg);
            if (r != (int)l->reg) {
                a64_emit("mov x%lu, x%d", (unsigned long)l->reg, r);
            }
        }
    }
    if (c->sret_addr >= 0) {
        int r = a64_use_x(c->sret_addr, 8);
        if (r != 8) {
            a64_emit("mov x8, x%d", r);
        }
    }
    if (tail) {
        /* the callee's address in x17, which the epilogue leaves alone; then the epilogue, and a jump */
        if (c->target >= 0) {
            int r = a64_use_x(c->target, 17);
            if (r != 17) {
                a64_emit("mov x17, x%d", r);
            }
        }
        if (!a64_leaf) {
            a64_saved_regs(1);
            a64_emit("mov sp, x29");
            a64_emit("ldp x29, x30, [sp], #16");
        }
        if (c->name) {
            a64_emit("b _%s", c->name);
        } else {
            a64_emit("br x17");
        }
        free(locs);
        return 1;
    }
    if (c->target >= 0) {
        int r = a64_use_x(c->target, 16);
        if (r != 16) {
            a64_emit("mov x16, x%d", r);
        }
    }
    if (c->name) {
        a64_emit("bl _%s", c->name);
    } else {
        a64_emit("blr x16");
    }
    adjust_sp("add", area);
    sp_disp = 0;
    free(locs);

    /* the result */
    if (ins->dst >= 0) {
        IRType t = a64_vtype(ins->dst);
        if (ir_type_is_float(t)) {
            def_done_d(ins->dst, 0);
        } else if (t == IR_I128) {
            a64_def_pair_done(ins->dst, 0, 1);
        } else {
            a64_def_done_x(ins->dst, 0);
        }
    } else if (c->result_addr >= 0) {
        int r = a64_use_x(c->result_addr, 16);
        const ArgType *t = &c->ret;
        if (t->hfa) {
            for (m = 0; m < t->hfa; m++) {
                a64_emit("str %c%lu, [x%d, #%lu]", t->hfa_size == 4 ? 's' : 'd', (unsigned long)m, r, (unsigned long)(m * t->hfa_size));
            }
        } else {
            a64_emit("str x0, [x%d]", r);
            if (t->size > 8) {
                a64_emit("str x1, [x%d, #8]", r);
            }
        }
    }
    return 0;
}

/* a return: the value to x0 / d0 / x0:x1, or a record's eightbytes, then the epilogue */
static void a64_select_ret(const IRInst *i) {
    if (i->flags & IRF_RECORD) {
        const ArgType *t = &a64_F->argtypes[i->imm];
        int r = a64_use_x(i->a, 16);
        size_t k;
        if (t->hfa) {
            for (k = 0; k < t->hfa; k++) {
                a64_emit("ldr %c%lu, [x%d, #%lu]", t->hfa_size == 4 ? 's' : 'd', (unsigned long)k, r, (unsigned long)(k * t->hfa_size));
            }
        } else {
            if (t->size > 8) {
                a64_load_word(1, r, 8, t->size - 8);
            }
            a64_load_word(0, r, 0, t->size >= 8 ? 8 : t->size);
        }
    } else if (i->a >= 0) {
        IRType t = a64_vtype(i->a);
        if (ir_type_is_float(t)) {
            int r = use_d(i->a, 0);
            if (r != 0) {
                a64_emit("fmov d0, d%d", r);
            }
        } else if (t == IR_I128) {
            a64_use_pair(i->a, 0, 1);
        } else {
            int r = a64_use_x(i->a, 0);
            if (r != 0) {
                a64_emit("mov x0, x%d", r);
            }
        }
    }
    a64_epilogue();
}

/* variadic callees: a va_list is a pointer into the caller's outgoing argument area */
static void a64_select_vaarg(const IRInst *i) {
    const ArgType *t = &a64_F->argtypes[i->aux];
    size_t size = t->size;
    size_t align = t->align;
    int is_record = t->kind == ARG_RECORD || t->kind == ARG_RECORD_MEMORY;
    size_t slot = (size + 7) & ~(size_t)7;
    int ap = a64_use_x(i->a, 9);

    a64_emit("ldr x1, [x%d]", ap);
    if (align > 8) {
        a64_emit("add x1, x1, #%lu", (unsigned long)(align - 1));
        a64_emit("and x1, x1, #-%lu", (unsigned long)align);
    }
    if (is_record && size > 16) {
        /* a composite over 16 bytes is passed by reference */
        a64_emit("ldr x16, [x1]");
        a64_emit("add x1, x1, #8");
        a64_emit("str x1, [x%d]", ap);
        a64_def_done_x(i->dst, 16);
        return;
    }
    a64_emit("add x16, x1, #%lu", (unsigned long)slot);
    a64_emit("str x16, [x%d]", ap);
    a64_def_done_x(i->dst, 1);
}

/* the branch at the end of a block; `next` is the block emitted after this one, which needs no jump */
static void a64_branch_to(int target, int next) {
    if (target != next) {
        a64_emit("b %s", a64_blabel(target));
    }
}

/* b.cond to `bt`, else to `bf` */
static void a64_cond_branch(const char *cond, int bt, int bf, int next) {
    if (bf == next) {
        a64_emit("b.%s %s", cond, a64_blabel(bt));
    } else if (bt == next) {
        a64_emit("b.%s %s", invert_cond(cond), a64_blabel(bf));
    } else {
        a64_emit("b.%s %s", cond, a64_blabel(bt));
        a64_emit("b %s", a64_blabel(bf));
    }
}

/* set the flags for an integer comparison; returns the condition to test */
static const char *a64_compare(const IRInst *i) {
    IRType t = (IRType)i->type;
    int a;
    if (t == IR_I128) {
        a64_use_pair(i->a, 9, 10);
        a64_use_pair(i->b, 1, 2);
        return a64_wide_compare((IRCond)i->cc);
    }
    a = a64_use_x(i->a, 9);
    if (i->flags & IRF_IMM) {
        long v = i->imm;
        if (v >= 0 && v <= 4095) {
            a64_emit("cmp %s, #%ld", rn(a, t), v);
        } else if (v < 0 && v >= -4095) {
            a64_emit("cmn %s, #%ld", rn(a, t), -v);
        } else {
            load_imm("x10", a64_imm32(t, v));
            a64_emit("cmp %s, %s", rn(a, t), rn(10, t));
        }
    } else {
        int b = a64_use_x(i->b, 10);
        a64_emit("cmp %s, %s", rn(a, t), rn(b, t));
    }
    return icond((IRCond)i->cc);
}

static const char *a64_fcompare(const IRInst *i) {
    IRType t = (IRType)i->type;
    int a = use_d(i->a, FS0);
    int b = use_d(i->b, FS1);
    a64_emit("fcmp %s, %s", fn(a, t), fn(b, t));
    return fcond((IRCond)i->cc);
}

/* at -O1, a floating point constant in one instruction: zero from the zero register, or fmov's 8 bit immediate */
static int a64_fconst_short(int d, IRType t, long imm) {
    double v;
    unsigned long bits;
    unsigned long rep;
    char buf[48];
    if (t == IR_F32) {
        float f;
        unsigned u = (unsigned)imm;
        memcpy(&f, &u, sizeof(f));
        v = f;
    } else {
        memcpy(&v, &imm, sizeof(v));
    }
    memcpy(&bits, &v, sizeof(bits));
    if (bits == 0) {
        a64_emit(t == IR_F32 ? "fmov s%d, wzr" : "fmov d%d, xzr", d);
        return 1;
    }
    rep = (bits >> 54) & 0xff;
    if ((bits & 0xffffffffffffUL) != 0 || (rep != 0 && rep != 0xff) || ((bits >> 62) & 1) == ((bits >> 61) & 1)
            || (rep & 1) != ((bits >> 61) & 1)) {
        return 0;
    }
    /* the value has at most 7 decimals (it is a multiple of 1/128 below 32), so %.7f is exact */
    snprintf(buf, sizeof(buf), "%.7f", v);
    a64_emit("fmov %c%d, #%s", t == IR_F32 ? 's' : 'd', d, buf);
    return 1;
}

/* one instruction; returns 1 when it consumed the next one too (a compare fused with its branch) */
static int a64_select_inst(int index, int next_block) {
    const IRInst *i = &a64_F->insts[index];
    IRType t = (IRType)i->type;
    IROp op = (IROp)i->op;
    char buf[32];
    int a, b, d;

    if (a64_folded[index]) {
        return 0;
    }
    if (i->dst >= 0 && a64_vtype(i->dst) == IR_I128 && op != IR_CALL && op != IR_FTOI && op != IR_TRUNC) {
        a64_select_wide(i);
        return 0;
    }
    if (op == IR_STORE && t == IR_I128) {
        a64_select_wide(i);
        return 0;
    }
    if (a64_combine[index] >= A64_C_FMADD_A) {
        /* the fused multiply-adds: x * z, then y added or subtracted */
        const IRInst *p = &a64_F->insts[index - 1];
        int k = a64_combine[index];
        int x = use_d(p->a, FS0);
        int z = use_d(p->b, FS1);
        int y = use_d(k == A64_C_FMADD_A || k == A64_C_FNMSUB ? i->b : i->a, FS2);
        d = a64_def_reg(i->dst, FS2);
        a64_emit("%s %s, %s, %s, %s", k == A64_C_FMSUB ? "fmsub" : k == A64_C_FNMSUB ? "fnmsub" : "fmadd",
             fn(d, t), fn(x, t), fn(z, t), fn(y, t));
        def_done_d(i->dst, d);
        return 0;
    }
    if (a64_combine[index]) {
        const IRInst *p = &a64_F->insts[index - 1];
        d = a64_def_reg(i->dst, 11);
        switch (a64_combine[index]) {
            case A64_C_UBFX:
                a = a64_use_x(p->a, 9);
                a64_emit("ubfx x%d, x%d, #%ld, #%d", d, a, p->imm & (t == IR_I32 ? 31 : 63), a64_mask_bits((unsigned long)i->imm, t));
                break;
            case A64_C_FIELD:
                if (a64_combine[index - 1] == A64_C_UBFX) {
                    const IRInst *q = &a64_F->insts[index - 2];
                    a = a64_use_x(q->a, 9);
                    a64_emit("ubfx x%d, x%d, #%ld, #%d", d, a, q->imm & 31, a64_mask_bits((unsigned long)p->imm, IR_I32));
                } else {
                    a = a64_use_x(p->a, 9);
                    a64_emit("and x%d, x%d, #0x%lx", d, a, (unsigned long)p->imm & 0xffffffffUL);
                }
                break;
            case A64_C_MADD_A:
            case A64_C_MADD_B: {
                int z, y;
                a = a64_use_x(p->a, 9);
                if (p->flags & IRF_IMM) {
                    load_imm("x10", a64_imm32(t, p->imm));
                    z = 10;
                } else {
                    z = a64_use_x(p->b, 10);
                }
                y = a64_use_x(a64_combine[index] == A64_C_MADD_A ? i->b : i->a, 16);
                a64_emit("madd %s, %s, %s, %s", rn(d, t), rn(a, t), rn(z, t), rn(y, t));
                break;
            }
            default:
                a = a64_use_x(p->a, 9);
                b = a64_use_x(a64_combine[index] == A64_C_SHIFT_A ? i->b : i->a, 10);
                a64_emit("add x%d, x%d, x%d, lsl #%ld", d, b, a, p->imm & 63);
                break;
        }
        a64_def_done_x(i->dst, d);
        return 0;
    }
    switch (op) {
        case IR_NOP:
            return 0;

        case IR_ICONST:
            d = a64_def_reg(i->dst, 11);
            load_imm(xn(d), a64_imm32(t, i->imm));
            a64_def_done_x(i->dst, d);
            return 0;

        case IR_FCONST:
            d = a64_def_reg(i->dst, FS2);
            if (a64_F->opt_level > 0 && a64_fconst_short(d, t, i->imm)) {
                def_done_d(i->dst, d);
                return 0;
            }
            if (t == IR_F32) {
                load_imm("x16", (unsigned long)(unsigned)i->imm);
                a64_emit("fmov s%d, w16", d);
            } else {
                load_imm("x16", (unsigned long)i->imm);
                a64_emit("fmov d%d, x16", d);
            }
            def_done_d(i->dst, d);
            return 0;

        case IR_ADDR_LOCAL:
            d = a64_def_reg(i->dst, 11);
            fp_addr(xn(d), i->imm);
            a64_def_done_x(i->dst, d);
            return 0;

        case IR_ADDR_GLOBAL:
            d = a64_def_reg(i->dst, 11);
            global_addr(xn(d), i->sym, i->flags & IRF_EXTERN);
            a64_def_done_x(i->dst, d);
            return 0;

        case IR_ADDR_STRING:
            d = a64_def_reg(i->dst, 11);
            a64_emit("adrp x%d, L.str.%ld@PAGE", d, i->imm);
            a64_emit("add x%d, x%d, L.str.%ld@PAGEOFF", d, d, i->imm);
            a64_def_done_x(i->dst, d);
            return 0;

        case IR_MOV:
            if (a64_loc_reg(i->a) < 0 && a64_loc_reg(i->dst) < 0 && a64_F->vregs[i->a].offset == a64_F->vregs[i->dst].offset) {
                return 0;   /* the two share a slot (src/regalloc.c merged them), or the destination is never read */
            }
            if (ir_type_is_float(t)) {
                a = use_d(i->a, FS0);
                d = a64_def_reg(i->dst, a);
                if (d != a) {
                    a64_emit("fmov d%d, d%d", d, a);
                }
                def_done_d(i->dst, d);
            } else {
                a = a64_use_x(i->a, 9);
                d = a64_def_reg(i->dst, a);
                if (d != a) {
                    a64_emit("mov x%d, x%d", d, a);
                }
                a64_def_done_x(i->dst, d);
            }
            return 0;

        case IR_LOAD: {
            const char *m = a64_mem_operand(index, 9, buf, sizeof(buf));
            if (ir_type_is_float(t)) {
                d = a64_def_reg(i->dst, FS0);
                a64_emit("ldr %s, [%s]", fn(d, t), m);
                def_done_d(i->dst, d);
            } else {
                d = a64_def_reg(i->dst, 11);
                if (t == IR_I32 && i->size == 4) {
                    a64_emit("ldr w%d, [%s]", d, m);
                } else if (t == IR_I32 && i->size < 4) {
                    a64_emit("%s w%d, [%s]", i->size == 1 ? (i->flags & IRF_SIGNED ? "ldrsb" : "ldrb") : (i->flags & IRF_SIGNED ? "ldrsh" : "ldrh"), d, m);
                } else {
                    load_sized(d, i->size, i->flags & IRF_SIGNED, m);
                }
                a64_def_done_x(i->dst, d);
            }
            return 0;
        }

        case IR_STORE:
            if (ir_type_is_float(t)) {
                const char *m;
                b = use_d(i->b, FS0);
                m = a64_mem_operand(index, 9, buf, sizeof(buf));
                a64_emit("str %s, [%s]", i->size == 4 ? sn(b) : dn(b), m);
            } else {
                const char *m;
                b = a64_use_x(i->b, 10);
                m = a64_mem_operand(index, 9, buf, sizeof(buf));
                store_sized(b, i->size, m);
            }
            return 0;

        case IR_LOADBITS: {
            const char *m = a64_mem_operand(index, 9, buf, sizeof(buf));
            d = a64_def_reg(i->dst, 11);
            load_sized(d, i->size, 0, m);
            a64_emit("%s x%d, x%d, #%ld, #%ld", i->flags & IRF_SIGNED ? "sbfx" : "ubfx", d, d, i->imm & 0xff, i->imm >> 8);
            a64_def_done_x(i->dst, d);
            return 0;
        }

        case IR_STOREBITS: {
            const char *m;
            b = a64_use_x(i->b, 10);
            m = a64_mem_operand(index, 9, buf, sizeof(buf));
            load_sized(16, i->size, 0, m);
            a64_emit("bfi x16, x%d, #%ld, #%ld", b, i->imm & 0xff, i->imm >> 8);
            store_sized(16, i->size, m);
            if (i->dst >= 0) {
                d = a64_def_reg(i->dst, 11);
                a64_emit("%s x%d, x16, #%ld, #%ld", i->flags & IRF_SIGNED ? "sbfx" : "ubfx", d, i->imm & 0xff, i->imm >> 8);
                a64_def_done_x(i->dst, d);
            }
            return 0;
        }

        case IR_COPY:
            a = a64_use_x(i->a, 9);
            b = a64_use_x(i->b, 10);
            a64_copy_bytes(a, b, (size_t)i->imm);
            return 0;

        case IR_ZERO:
            a = a64_use_x(i->a, 9);
            a64_zero_bytes(a, (size_t)i->imm);
            return 0;

        case IR_ADD: case IR_SUB: case IR_MUL: case IR_SDIV: case IR_UDIV: case IR_SREM: case IR_UREM:
        case IR_SHL: case IR_LSHR: case IR_ASHR: case IR_AND: case IR_OR: case IR_XOR:
            a = a64_use_x(i->a, 9);
            d = a64_def_reg(i->dst, 11);
            if (i->flags & IRF_IMM) {
                binop_imm(op, t, d, a, i->imm);
            } else {
                b = a64_use_x(i->b, 10);
                binop_reg(op, t, d, a, b);
            }
            a64_def_done_x(i->dst, d);
            return 0;

        case IR_NEG:
            a = a64_use_x(i->a, 9);
            d = a64_def_reg(i->dst, 11);
            a64_emit("neg %s, %s", rn(d, t), rn(a, t));
            a64_def_done_x(i->dst, d);
            return 0;

        case IR_NOT:
            a = a64_use_x(i->a, 9);
            d = a64_def_reg(i->dst, 11);
            a64_emit("mvn %s, %s", rn(d, t), rn(a, t));
            a64_def_done_x(i->dst, d);
            return 0;

        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV:
            a = use_d(i->a, FS0);
            b = use_d(i->b, FS1);
            d = a64_def_reg(i->dst, FS2);
            a64_emit("%s %s, %s, %s", op == IR_FADD ? "fadd" : op == IR_FSUB ? "fsub" : op == IR_FMUL ? "fmul" : "fdiv",
                 fn(d, t), fn(a, t), fn(b, t));
            def_done_d(i->dst, d);
            return 0;

        case IR_FNEG:
        case IR_FSQRT:
        case IR_FABS:
        case IR_FRINT:
            a = use_d(i->a, FS0);
            d = a64_def_reg(i->dst, FS2);
            a64_emit("%s %s, %s", op == IR_FNEG ? "fneg" : op == IR_FSQRT ? "fsqrt" : op == IR_FABS ? "fabs"
                     : i->imm == 0 ? "frintm" : i->imm == 1 ? "frintp" : i->imm == 2 ? "frintz" : "frinta", fn(d, t), fn(a, t));
            def_done_d(i->dst, d);
            return 0;

        case IR_ICMP:
        case IR_FCMP: {
            const char *cond = op == IR_ICMP ? a64_compare(i) : a64_fcompare(i);
            const IRInst *n = index + 1 < a64_F->ninsts ? &a64_F->insts[index + 1] : NULL;
            if (n && n->op == IR_CONDBR && n->a == i->dst && a64_F->vregs[i->dst].nuses == 1) {
                a64_cond_branch(cond, (int)n->imm, n->aux, next_block);
                return 1;
            }
            d = a64_def_reg(i->dst, 11);
            a64_emit("cset w%d, %s", d, cond);
            a64_def_done_x(i->dst, d);
            return 0;
        }

        case IR_SEXT:
        case IR_ZEXT:
            a = a64_use_x(i->a, 9);
            d = a64_def_reg(i->dst, 11);
            if (op == IR_SEXT) {
                a64_emit("%s %s, w%d", i->size == 1 ? "sxtb" : i->size == 2 ? "sxth" : "sxtw", rn(d, t), a);
            } else if (i->size == 4) {
                a64_emit("mov w%d, w%d", d, a);
            } else {
                a64_emit("and %s, %s, #0x%x", rn(d, t), rn(a, t), i->size == 1 ? 0xff : 0xffff);
            }
            a64_def_done_x(i->dst, d);
            return 0;

        case IR_TRUNC:
            if (a64_vtype(i->a) == IR_I128) {
                a64_use_pair(i->a, 9, 10);
                a64_def_done_x(i->dst, 9);
            } else {
                a = a64_use_x(i->a, 9);
                d = a64_def_reg(i->dst, a);
                if (d != a) {
                    a64_emit("mov w%d, w%d", d, a);
                }
                a64_def_done_x(i->dst, d);
            }
            return 0;

        case IR_ITOF:
            if (i->t2 == IR_I128) {
                a64_use_pair(i->a, 0, 1);
                a64_emit("bl _%s", t == IR_F32 ? (i->flags & IRF_SIGNED ? "__floattisf" : "__floatuntisf")
                                           : (i->flags & IRF_SIGNED ? "__floattidf" : "__floatuntidf"));
                def_done_d(i->dst, 0);
                return 0;
            }
            a = a64_use_x(i->a, 9);
            d = a64_def_reg(i->dst, FS2);
            a64_emit("%s %s, %s", i->flags & IRF_SIGNED ? "scvtf" : "ucvtf", fn(d, t), rn(a, (IRType)i->t2));
            def_done_d(i->dst, d);
            return 0;

        case IR_FTOI:
            if (t == IR_I128) {
                a = use_d(i->a, 0);
                if (a != 0) {
                    a64_emit("fmov d0, d%d", a);
                }
                a64_emit("bl _%s", i->t2 == IR_F32 ? (i->flags & IRF_SIGNED ? "__fixsfti" : "__fixunssfti")
                                               : (i->flags & IRF_SIGNED ? "__fixdfti" : "__fixunsdfti"));
                a64_def_pair_done(i->dst, 0, 1);
                return 0;
            }
            a = use_d(i->a, FS0);
            d = a64_def_reg(i->dst, 11);
            a64_emit("%s %s, %s", i->flags & IRF_SIGNED ? "fcvtzs" : "fcvtzu", rn(d, t), fn(a, (IRType)i->t2));
            a64_def_done_x(i->dst, d);
            return 0;

        case IR_FTOF:
            a = use_d(i->a, FS0);
            d = a64_def_reg(i->dst, FS2);
            a64_emit("fcvt %s, %s", fn(d, t), fn(a, (IRType)i->t2));
            def_done_d(i->dst, d);
            return 0;

        case IR_BITCAST:
            if (ir_type_is_float(t)) {
                a = a64_use_x(i->a, 9);
                d = a64_def_reg(i->dst, FS2);
                a64_emit("fmov %s, %s", fn(d, t), rn(a, (IRType)i->t2));
                def_done_d(i->dst, d);
            } else {
                a = use_d(i->a, FS0);
                d = a64_def_reg(i->dst, 11);
                a64_emit("fmov %s, %s", rn(d, t), fn(a, (IRType)i->t2));
                a64_def_done_x(i->dst, d);
            }
            return 0;

        case IR_BSWAP:
            a = a64_use_x(i->a, 9);
            d = a64_def_reg(i->dst, 11);
            switch (i->size) {
                case 2: a64_emit("rev16 w%d, w%d", d, a); break;
                case 4: a64_emit("rev w%d, w%d", d, a); break;
                default: a64_emit("rev x%d, x%d", d, a); break;
            }
            a64_def_done_x(i->dst, d);
            return 0;

        case IR_CLZ:
            a = a64_use_x(i->a, 9);
            d = a64_def_reg(i->dst, 11);
            a64_emit("clz %s, %s", rn(d, t), rn(a, t));
            a64_def_done_x(i->dst, d);
            return 0;

        case IR_CTZ:
            a = a64_use_x(i->a, 9);
            d = a64_def_reg(i->dst, 11);
            a64_emit("rbit %s, %s", rn(d, t), rn(a, t));
            a64_emit("clz %s, %s", rn(d, t), rn(d, t));
            a64_def_done_x(i->dst, d);
            return 0;

        case IR_FENCE:
            a64_emit("dmb ish");
            return 0;

        case IR_ALLOCA:
            a = a64_use_x(i->a, 9);
            d = a64_def_reg(i->dst, 11);
            a64_emit("add x16, x%d, #15", a);
            a64_emit("and x16, x16, #0xfffffffffffffff0");
            a64_emit("sub sp, sp, x16");
            a64_emit("mov x%d, sp", d);
            a64_def_done_x(i->dst, d);
            return 0;

        case IR_BR:
            a64_branch_to((int)i->imm, next_block);
            return 0;

        case IR_CONDBR:
            a = a64_use_x(i->a, 9);
            if (i->aux == next_block) {
                a64_emit("cbnz %s, %s", rn(a, t), a64_blabel((int)i->imm));
            } else if ((int)i->imm == next_block) {
                a64_emit("cbz %s, %s", rn(a, t), a64_blabel(i->aux));
            } else {
                a64_emit("cbnz %s, %s", rn(a, t), a64_blabel((int)i->imm));
                a64_emit("b %s", a64_blabel(i->aux));
            }
            return 0;

        case IR_RET:
            a64_select_ret(i);
            return 0;

        case IR_UNREACHABLE:
            a64_emit("brk #1");
            return 0;

        case IR_SWITCH: {
            /* a branch table: the index's entry is a b to its case */
            const IRSwitch *sw = &a64_F->switches[i->imm];
            int k, l = ++a64_nlabels;
            a = a64_use_x(i->a, 9);
            if (sw->n <= 4095) {
                a64_emit("cmp x%d, #%d", a, sw->n);
            } else {
                load_imm("x10", (unsigned long)sw->n);
                a64_emit("cmp x%d, x10", a);
            }
            a64_emit("b.hs %s", a64_blabel(i->aux));
            a64_emit("adr x16, Lcg%d", l);
            a64_emit("add x16, x16, x%d, lsl #2", a);
            a64_emit("br x16");
            peep_flush();
            fprintf(a64_cg_out, "Lcg%d:\n", l);
            for (k = 0; k < sw->n; k++) {
                a64_emit("b %s", a64_blabel(a64_F->jt[sw->first + k]));
            }
            peep_flush();
            return 0;
        }

        case IR_CALL:
            return a64_select_call(index);

        case IR_VASTART:
            a = a64_use_x(i->a, 9);
            fp_addr("x1", a64_cg_stack_param_offset(a64_round_up(a64_F->nstack_params, 8)));
            a64_emit("str x1, [x%d]", a);
            return 0;

        case IR_VAARG:
            a64_select_vaarg(i);
            return 0;

        default:
            return 0;
    }
}

/* the prologue: the frame, then the parameters that arrived in registers to where they live */
static void a64_prologue(void) {
    int k;
    size_t m;
    char buf[32];

    a64_emit(".text");
    if (a64_F->is_global) {
        a64_emit(".globl _%s", a64_F->name);
    }
    a64_emit(".p2align 2");
    peep_flush();
    fprintf(a64_cg_out, "_%s:\n", a64_F->name);
    a64_emit(".cfi_startproc");
    if (!a64_leaf) {
        a64_emit("stp x29, x30, [sp, #-16]!");
        a64_emit("mov x29, sp");
    }
    adjust_sp("sub", (size_t)a64_F->frame_size);
    a64_saved_regs(0);
    if (a64_F->sret_off) {
        a64_emit("str x8, [%s]", frame_operand(a64_F->sret_off, 8, buf, sizeof(buf)));
    }
    for (k = 0; k < a64_F->nparams; k++) {
        const IRParam *p = &a64_F->params[k];
        const ArgLoc *l = &p->loc;
        if (!l->in_regs) {
            continue;
        }
        if (p->kr_from) {
            /* a K&R float parameter: the double it arrived as, to its slot; the entry block converts it */
            a64_emit("str d%lu, [%s]", (unsigned long)l->freg, frame_operand(p->kr_from, 8, buf, sizeof(buf)));
        } else if (p->vreg >= 0) {
            if (p->is_float) {
                def_done_d(p->vreg, (int)l->freg);
            } else if (p->wide) {
                a64_def_pair_done(p->vreg, (int)l->reg, (int)l->reg + 1);
            } else {
                if (p->size < 4) {
                    /* the caller may not have extended a narrow argument */
                    a64_emit("%s w%lu, w%lu", p->size == 1 ? (p->is_signed ? "sxtb" : "uxtb") : (p->is_signed ? "sxth" : "uxth"),
                         (unsigned long)l->reg, (unsigned long)l->reg);
                }
                a64_def_done_x(p->vreg, (int)l->reg);
            }
        } else if (l->by_ref) {
            a64_emit("str x%lu, [%s]", (unsigned long)l->reg, frame_operand(p->offset, 8, buf, sizeof(buf)));
        } else if (p->by_addr) {
            /* eightbytes, or an HFA's members, each from its own register */
            fp_addr("x16", p->offset);
            if (l->type.hfa) {
                for (m = 0; m < l->type.hfa; m++) {
                    a64_emit("str %c%lu, [x16, #%lu]", l->type.hfa_size == 4 ? 's' : 'd', (unsigned long)(l->freg + m),
                         (unsigned long)(m * l->type.hfa_size));
                }
            } else {
                for (m = 0; m < l->nregs; m++) {
                    a64_emit("str x%lu, [x16, #%lu]", (unsigned long)(l->reg + m), (unsigned long)(8 * m));
                }
            }
        } else if (p->wide) {
            fp_addr("x16", p->offset);
            a64_emit("stp x%lu, x%lu, [x16]", (unsigned long)l->reg, (unsigned long)(l->reg + 1));
        } else if (p->is_float) {
            a64_emit("str %c%lu, [%s]", p->size == 4 ? 's' : 'd', (unsigned long)l->freg, frame_operand(p->offset, (int)p->size, buf, sizeof(buf)));
        } else {
            store_sized((int)l->reg, p->size, frame_operand(p->offset, (int)p->size, buf, sizeof(buf)));
        }
    }
}

/* a block that is nothing but a jump is skipped by the branches to it, which leaves it unreachable and unemitted */
static void a64_thread_jumps(void) {
    int b, i, k;
    for (i = 0; i < a64_F->ninsts; i++) {
        IRInst *t = &a64_F->insts[i];
        for (k = 0; k < ir_nsuccs(a64_F, t); k++) {
            int hops = 0;
            b = ir_succ(a64_F, t, k);
            while (hops++ < 64 && a64_F->blocks[b].start >= 0 && a64_F->blocks[b].end == a64_F->blocks[b].start + 1
                    && a64_F->insts[a64_F->blocks[b].start].op == IR_BR && (int)a64_F->insts[a64_F->blocks[b].start].imm != b) {
                b = (int)a64_F->insts[a64_F->blocks[b].start].imm;
            }
            ir_set_succ(a64_F, t, k, b);
        }
    }
}

static void a64_cg_select(IRFunc *f) {
    int b, i;
    int *next_of;

    a64_F = f;
    a64_fold_of = xmalloc((size_t)(f->ninsts + 1) * sizeof(int));
    a64_folded = xmalloc((size_t)(f->ninsts + 1) * sizeof(int));
    a64_combine = xmalloc((size_t)(f->ninsts + 1) * sizeof(int));
    a64_block_start = xmalloc((size_t)(f->ninsts + 1) * sizeof(int));
    a64_npreds = xmalloc((size_t)(f->nblocks + 1) * sizeof(int));
    next_of = xmalloc((size_t)(f->nblocks + 1) * sizeof(int));
    a64_thread_jumps();
    a64_find_folds();
    regalloc(f, &a64_regtarget);
    a64_leaf = a64_is_leaf();
    frame_size = f->frame_size;
    sp_disp = 0;
    sp_known = 1;
    for (i = 0; i < f->ninsts; i++) {
        if (f->insts[i].op == IR_ALLOCA) {
            sp_known = 0;
        }
    }
    /* blocks nothing reaches are not emitted; the rest in order, each falling through to the next emitted one */
    for (b = 0; b < f->nblocks; b++) {
        a64_npreds[b] = 0;
    }
    for (b = 0; b < f->nblocks; b++) {
        const IRInst *t;
        if (f->blocks[b].start < 0 || f->blocks[b].end <= f->blocks[b].start) {
            continue;
        }
        t = &f->insts[f->blocks[b].end - 1];
        for (i = 0; i < ir_nsuccs(f, t); i++) {
            a64_npreds[ir_succ(f, t, i)]++;
        }
    }
    {
        int next = -1;
        for (b = f->nblocks - 1; b >= 0; b--) {
            next_of[b] = next;
            if (f->blocks[b].start >= 0 && (b == 0 || a64_npreds[b] > 0)) {
                next = b;
            }
        }
    }
    a64_label_base = a64_nlabels + 1;
    a64_nlabels += f->nblocks;

    a64_prologue();
    for (b = 0; b < f->nblocks; b++) {
        const IRBlock *blk = &f->blocks[b];
        if (blk->start < 0 || (b != 0 && a64_npreds[b] == 0)) {
            continue;
        }
        if (b != 0 || a64_npreds[b] > 0) {
            peep_flush();
            fprintf(a64_cg_out, "%s:\n", a64_blabel(b));
        }
        for (i = blk->start; i < blk->end; i++) {
            i += a64_select_inst(i, next_of[b]);
        }
    }
    a64_emit(".cfi_endproc");
    free(a64_fold_of);
    free(a64_folded);
    free(a64_combine);
    free(a64_block_start);
    a64_combine = a64_block_start = NULL;
    free(a64_npreds);
    free(next_of);
    a64_fold_of = a64_folded = a64_npreds = NULL;
    a64_F = NULL;
}

/* preprocessing for Apple arm64: clang's predefined macros and the macOS SDK */

#include "cpp.h"
#include "predef_arm64_macos.h"

static const char *const a64_system_dirs[] = {
    JCC_ARM64_MACOS_RESOURCE_INC,   /* the compiler's own headers: stddef.h, stdarg.h, ... */
    "=/usr/include",
    NULL
};

static const char *const a64_has_builtins[] = {
    "__has_feature", "__has_extension", "__has_attribute", "__has_builtin",
    "__has_cpp_attribute", "__has_c_attribute", "__has_declspec_attribute",
    "__has_warning", "__has_include", "__has_include_next",
    "__is_target_arch", "__is_target_vendor", "__is_target_os", "__is_target_environment",
    "__is_target_variant_os", "__is_target_variant_environment",
    "__building_module", "__is_identifier",
    NULL
};

static const CppTarget a64_cpp_target = {
    JCC_ARM64_MACOS_SYSROOT,
    a64_system_dirs,
    a64_predefined_macros,
    a64_nonstrict_macros,
    a64_has_answers,
    a64_has_builtins
};

#include "target.h"

static const char *const a64_aliases[] = {
    "aarch64-apple-darwin", "arm64-apple-darwin", "arm64", "aarch64", NULL
};

/* the primitives, in the order of struct Backend */
static const Backend a64_backend = {
    a64_cg_begin,
    a64_cg_end,
    a64_cg_data_begin,
    a64_cg_data_int,
    a64_cg_data_addr,
    a64_cg_data_string_addr,
    a64_cg_data_bytes,
    a64_cg_data_zero,
    a64_cg_string,
    a64_cg_stack_param_offset,
    a64_cg_args_begin,
    a64_cg_args_next,
    a64_cg_args_end,
    a64_cg_record_ret_regs,
    a64_cg_select
};

/* no TargetLink: a Mach-O link needs ld64, so from another host use -c */
const Target target_arm64_macos = { "arm64-apple-macos", a64_aliases, &a64_info, &a64_cpp_target, &a64_backend, NULL, &as_arm64 };
