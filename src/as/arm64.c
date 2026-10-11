/*
 * The arm64 half of the assembler: register names, operand syntax, and
 * the table of instruction forms the arm64 backend emits (tools/as-corpus.sh
 * collects the corpus the table was read off). The operand alphabet of
 * the table's shapes:
 *
 *   X  x0-x30        W  w0-w30        P  sp     Z  xzr    z  wzr
 *   D  d0-d31        S  s0-s31
 *   I  #imm
 *   L  a label or symbol      G  sym@PAGE      O  sym@PAGEOFF    g  sym@GOTPAGE
 *   m  [Xn] or [Xn, #imm]     p  [Xn, #imm]!   q  [Xn], #imm     r  [Xn, Xm]    h  [Xn, sym@GOTPAGEOFF]
 *   K  lsl #imm               C  a condition code (also the one in b.cond, as a first operand)    B  ish
 *
 * The statics both halves define (the AsmArch entries and the operand
 * parser) carry the arm64_ prefix here and x86_64_ in x86_64.c, so that
 * both can live in one translation unit, the amalgamation.
 */
#include <stdlib.h>
#include <string.h>

#include "asm.h"

enum { R_X, R_W, R_SP, R_XZR, R_WZR, R_D, R_S };

static const char *const conds[16] = {
    "eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"
};

static int cond_code(const char *s, size_t len) {
    int i;
    if (len != 2) {
        return -1;
    }
    for (i = 0; i < 16; i++) {
        if (memcmp(conds[i], s, 2) == 0) {
            return i;
        }
    }
    if (memcmp("cs", s, 2) == 0) return 2;
    if (memcmp("cc", s, 2) == 0) return 3;
    return -1;
}

/* a register name: 0 and the class and number, or -1 */
static int reg_name(const AsmTok *t, int *cls, int *num) {
    const char *s;
    size_t len;
    int max = 30;
    int n;
    size_t i;
    if (t->kind != ATOK_IDENT) {
        return -1;
    }
    s = t->s;
    len = t->len;
    if (len == 2 && s[0] == 's' && s[1] == 'p') { *cls = R_SP; *num = 31; return 0; }
    if (len == 3 && s[0] == 'x' && s[1] == 'z' && s[2] == 'r') { *cls = R_XZR; *num = 31; return 0; }
    if (len == 3 && s[0] == 'w' && s[1] == 'z' && s[2] == 'r') { *cls = R_WZR; *num = 31; return 0; }
    if (len < 2 || len > 3) {
        return -1;
    }
    switch (s[0]) {
        case 'x': *cls = R_X; break;
        case 'w': *cls = R_W; break;
        case 'd': *cls = R_D; max = 31; break;
        case 's': *cls = R_S; max = 31; break;
        default: return -1;
    }
    n = 0;
    for (i = 1; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        n = n * 10 + (s[i] - '0');
    }
    if (len == 3 && s[1] == '0') {
        return -1;   /* x01 */
    }
    if (n > max) {
        return -1;
    }
    *num = n;
    return 0;
}

/* sym[@PAGE|@PAGEOFF|@GOTPAGE|@GOTPAGEOFF] at t[*i]: fills op, or -1 */
static int parse_sym(As *a, const AsmTok *t, size_t *i, AsmOp *op) {
    if (t[*i].kind != ATOK_IDENT) {
        return -1;
    }
    op->kind = OP_SYM;
    op->mode = SYM_PLAIN;
    op->sym = as_symbol(a, t[*i].s, t[*i].len);
    (*i)++;
    if (t[*i].kind == ATOK_PUNCT && t[*i].punct == '@') {
        const AsmTok *k = &t[*i + 1];
        if (as_tok_is(k, "PAGE")) op->mode = SYM_PAGE;
        else if (as_tok_is(k, "PAGEOFF")) op->mode = SYM_PAGEOFF;
        else if (as_tok_is(k, "GOTPAGE")) op->mode = SYM_GOTPAGE;
        else if (as_tok_is(k, "GOTPAGEOFF")) op->mode = SYM_GOTPAGEOFF;
        else return -1;
        *i += 2;
    }
    return 0;
}

/* #[-]n at t[*i] */
static int parse_imm(const AsmTok *t, size_t *i, long *v) {
    int neg = 0;
    if (t[*i].kind != ATOK_PUNCT || t[*i].punct != '#') {
        return -1;
    }
    (*i)++;
    if (t[*i].kind == ATOK_PUNCT && t[*i].punct == '-') {
        neg = 1;
        (*i)++;
    }
    if (t[*i].kind != ATOK_NUM) {
        return -1;
    }
    *v = neg ? -(long)t[*i].num : (long)t[*i].num;
    (*i)++;
    return 0;
}

static int arm64_is_punct(const AsmTok *t, char c) {
    return t->kind == ATOK_PUNCT && t->punct == c;
}

/* one operand at t[*i] */
static int arm64_parse_operand(As *a, const AsmTok *t, size_t *i, AsmOp *op) {
    int cls;
    int num;
    long v;

    memset(op, 0, sizeof(*op));
    op->base = op->index = NO_REG;
    op->sym = -1;
    if (reg_name(&t[*i], &cls, &num) == 0) {
        op->kind = OP_REG;
        op->cls = (unsigned char)cls;
        op->reg = (unsigned char)num;
        (*i)++;
        return 0;
    }
    if (arm64_is_punct(&t[*i], '#') && (t[*i + 1].kind == ATOK_FNUM
            || (arm64_is_punct(&t[*i + 1], '-') && t[*i + 2].kind == ATOK_FNUM))) {
        /* #2.5 or #-2.5: the double's bits, the sign flipped for a minus */
        int neg = arm64_is_punct(&t[*i + 1], '-');
        unsigned long bits = t[*i + 1 + neg].num;
        if (neg) {
            bits ^= 1UL << 63;
        }
        op->kind = OP_FIMM;
        op->imm = (long)bits;
        *i += 2 + (size_t)neg;
        return 0;
    }
    if (parse_imm(t, i, &v) == 0) {
        op->kind = OP_IMM;
        op->imm = v;
        return 0;
    }

    if (arm64_is_punct(&t[*i], '[')) {
        (*i)++;
        if (reg_name(&t[*i], &cls, &num) != 0 || (cls != R_X && cls != R_SP)) {
            return -1;
        }
        op->kind = OP_MEM;
        op->mode = MEM_BASE;
        op->base = (unsigned char)num;
        (*i)++;
        if (arm64_is_punct(&t[*i], ',')) {
            (*i)++;
            if (parse_imm(t, i, &v) == 0) {
                op->imm = v;
            } else if (reg_name(&t[*i], &cls, &num) == 0 && cls == R_X) {
                op->mode = MEM_INDEX;
                op->index = (unsigned char)num;
                (*i)++;
            } else if (t[*i].kind == ATOK_IDENT) {
                AsmOp s;
                if (parse_sym(a, t, i, &s) != 0 || s.mode != SYM_GOTPAGEOFF) {
                    return -1;
                }
                op->mode = MEM_SYM;
                op->sym = s.sym;
            } else {
                return -1;
            }
        }
        if (!arm64_is_punct(&t[*i], ']')) {
            return -1;
        }
        (*i)++;
        if (arm64_is_punct(&t[*i], '!')) {
            if (op->mode != MEM_BASE) {
                return -1;
            }
            op->mode = MEM_PRE;
            (*i)++;
        } else if (op->mode == MEM_BASE && op->imm == 0 && arm64_is_punct(&t[*i], ',') && arm64_is_punct(&t[*i + 1], '#')) {
            (*i)++;
            if (parse_imm(t, i, &v) != 0) {
                return -1;
            }
            op->mode = MEM_POST;
            op->imm = v;
        }
        return 0;
    }
    if (t[*i].kind == ATOK_IDENT) {
        if (as_tok_is(&t[*i], "lsl") || as_tok_is(&t[*i], "lsr") || as_tok_is(&t[*i], "asr")) {
            op->kind = OP_SHIFT;
            op->reg = (unsigned char)(t[*i].s[0] == 'l' ? (t[*i].s[2] == 'l' ? SHIFT_LSL : SHIFT_LSR) : SHIFT_ASR);
            (*i)++;
            if (parse_imm(t, i, &v) != 0) {
                return -1;
            }
            op->imm = v;
            return 0;
        }
        if (as_tok_is(&t[*i], "ish")) {
            op->kind = OP_BARRIER;
            (*i)++;
            return 0;
        }
        if ((num = cond_code(t[*i].s, t[*i].len)) >= 0) {
            op->kind = OP_COND;
            op->imm = num;
            (*i)++;
            return 0;
        }
        return parse_sym(a, t, i, op);
    }
    return -1;
}

static int arm64_parse(As *a, char *mnem, const AsmTok *t, size_t n, AsmInst *out) {
    size_t i = 0;
    out->nops = 0;
    if (mnem[0] == 'b' && mnem[1] == '.') {
        int c = cond_code(mnem + 2, strlen(mnem + 2));
        AsmOp *op = &out->ops[out->nops++];
        if (c < 0) {
            as_error(a, "unknown condition in %s", mnem);
            return -1;
        }
        memset(op, 0, sizeof(*op));
        op->kind = OP_COND;
        op->imm = c;
        op->sym = -1;
        mnem[2] = '\0';
    }
    while (i < n) {
        if (out->nops == MAX_OPS) {
            as_error(a, "too many operands");
            return -1;
        }
        if (arm64_parse_operand(a, t, &i, &out->ops[out->nops]) != 0) {
            as_error(a, "cannot parse operand %d of %s", out->nops + 1, mnem);
            return -1;
        }
        out->nops++;
        if (i < n) {
            if (!arm64_is_punct(&t[i], ',')) {
                as_error(a, "expected ',' after operand %d of %s", out->nops, mnem);
                return -1;
            }
            i++;
            if (i == n) {
                as_error(a, "missing operand after ',' in %s", mnem);
                return -1;
            }
        }
    }
    return 0;
}

static void arm64_shape(const AsmInst *in, char *buf) {
    size_t k;
    for (k = 0; k < in->nops; k++) {
        const AsmOp *op = &in->ops[k];
        char c = '?';
        switch (op->kind) {
            case OP_REG:
                switch (op->cls) {
                    case R_X: c = 'X'; break;
                    case R_W: c = 'W'; break;
                    case R_SP: c = 'P'; break;
                    case R_XZR: c = 'Z'; break;
                    case R_WZR: c = 'z'; break;
                    case R_D: c = 'D'; break;
                    case R_S: c = 'S'; break;
                }
                break;
            case OP_IMM: c = 'I'; break;
            case OP_FIMM: c = 'F'; break;
            case OP_SYM:
                switch (op->mode) {
                    case SYM_PLAIN: c = 'L'; break;
                    case SYM_PAGE: c = 'G'; break;
                    case SYM_PAGEOFF: c = 'O'; break;
                    case SYM_GOTPAGE: c = 'g'; break;
                }
                break;
            case OP_MEM:
                switch (op->mode) {
                    case MEM_BASE: c = 'm'; break;
                    case MEM_PRE: c = 'p'; break;
                    case MEM_POST: c = 'q'; break;
                    case MEM_INDEX: c = 'r'; break;
                    case MEM_SYM: c = 'h'; break;
                }
                break;
            case OP_SHIFT: c = op->reg == SHIFT_LSL ? 'K' : '?'; break;
            case OP_COND: c = 'C'; break;
            case OP_BARRIER: c = 'B'; break;
        }
        buf[k] = c;
    }
    buf[k] = '\0';
}


/* ------------------------------------------------------------------ */
/* encoding                                                            */
/* ------------------------------------------------------------------ */

/*
 * Each row of the table names an encoding family and the opcode bits
 * the family starts from (the fixed fields, and for aliases such as
 * `cmp` and `mul` the register fields that are always 31). The family
 * says which operand goes in which field. The reference is `cc -c` on
 * the corpus, which tests/as-obj.sh compares against byte for byte.
 */
enum {
    F_FIXED,        /* no fields */
    F_RR,           /* op | Rn<<5 | Rd */
    F_RM,           /* op | Rm<<16 | Rd (Rn fixed, as in mov, mvn, neg) */
    F_RRR,          /* op | Rm<<16 | Rn<<5 | Rd; with two operands Rd is fixed (cmp, fcmp) */
    F_RRR_SHIFT,    /* op | Rm<<16 | imm6<<10 | Rn<<5 | Rd: orr xd, xn, xm, lsl #n */
    F_MADD,         /* op | Rm<<16 | Ra<<10 | Rn<<5 | Rd */
    F_ADDSUB_IMM,   /* op | imm12<<10 | Rn<<5 | Rd; with two operands Rd is fixed (cmp, cmn) */
    F_LOGIMM,       /* op | N:immr:imms | Rn<<5 | Rd, the bitmask immediate */
    F_MOVW,         /* op | hw<<21 | imm16<<5 | Rd: movz, movn, movk */
    F_MOVI,         /* mov xd, #imm: movz or movn */
    F_LSL_IMM,      /* ubfm for lsl #n */
    F_SHR_IMM,      /* ubfm/sbfm for lsr/asr #n */
    F_BFI,          /* bfm for bfi lsb, width */
    F_BFX,          /* ubfm/sbfm for ubfx/sbfx lsb, width */
    F_CSET,         /* csinc wd, wzr, wzr, !cond */
    F_B,            /* b, bl: imm26 or a BRANCH26 relocation */
    F_BCOND,        /* b.cond: cond | imm19<<5 */
    F_CBZ,          /* cbz, cbnz: Rt | imm19<<5 */
    F_BLR,          /* op | Rn<<5 */
    F_BRK,          /* op | imm16<<5 */
    F_ADRP,         /* op | Rd and a PAGE21 or GOT_LOAD_PAGE21 relocation */
    F_ADR,          /* adr xd, label in this section: op | immlo<<29 | immhi<<5 | Rd */
    F_FMOVI,        /* fmov dd/sd, #fp: op | imm8<<13 | Rd, the 8 bit floating point immediate */
    F_PAGEOFF,      /* add xd, xn, sym@PAGEOFF: op | Rn<<5 | Rd and a PAGEOFF12 relocation */
    F_LDST,         /* loads and stores with [Xn, #imm]: scaled unsigned offset, else the unscaled form */
    F_LDST_REG,     /* ldrb/strb with [Xn, Xm] */
    F_LDP           /* ldp/stp: signed offset, pre-index or post-index */
};

static size_t arm64_insn_size(As *a, int sec, const AsmItem *it) {
    (void)a;
    (void)sec;
    (void)it;
    return 4;
}

static void arm64_pad(unsigned char *out, size_t n) {
    static const unsigned char nop[4] = { 0x1f, 0x20, 0x03, 0xd5 };
    size_t i;
    for (i = 0; i + 4 <= n; i += 4) {
        memcpy(out + i, nop, 4);
    }
}

/* the ones of v are contiguous (v is a mask shifted up) */
static int shifted_mask(unsigned long v) {
    unsigned long m = (v - 1) | v;
    return v != 0 && ((m + 1) & m) == 0;
}

/*
 * The bitmask immediate of the logical instructions: a run of ones in an
 * element of 2, 4, ..., 64 bits replicated across the register, as
 * N:immr:imms (13 bits) in `enc`. Returns -1 when `v` is not one.
 */
static int logical_imm(unsigned long v, int bits, unsigned long *enc) {
    int size;
    unsigned long mask;
    int i;
    int cto;
    int immr;
    unsigned long nimms;

    if (bits == 32) {
        v &= 0xfffffffful;
        v |= v << 32;
    }
    if (v == 0 || v == ~0ul) {
        return -1;
    }
    /* the smallest element that replicates */
    size = 64;
    do {
        size /= 2;
        mask = (1ul << size) - 1;
        if ((v & mask) != ((v >> size) & mask)) {
            size *= 2;
            break;
        }
    } while (size > 2);
    mask = size == 64 ? ~0ul : (1ul << size) - 1;
    v &= mask;
    if (shifted_mask(v)) {
        /* i zeros below the run of cto ones */
        for (i = 0; !((v >> i) & 1); i++) {
        }
        for (cto = 0; i + cto < 64 && ((v >> (i + cto)) & 1); cto++) {
        }
    } else {
        /* the run of ones wraps round: the zeros are contiguous */
        unsigned long nv;
        int clo;
        int ctz;
        v |= ~mask;
        nv = ~v;
        if (!shifted_mask(nv)) {
            return -1;
        }
        for (clo = 0; clo < 64 && ((v >> (63 - clo)) & 1); clo++) {
        }
        for (ctz = 0; ctz < 64 && !((nv >> ctz) & 1); ctz++) {
        }
        i = 64 - clo;
        cto = clo + ctz - (64 - size);
    }
    immr = (size - i) & (size - 1);
    nimms = (unsigned long)(~(size - 1)) << 1;
    nimms |= (unsigned long)(cto - 1);
    *enc = ((((nimms >> 6) & 1) ^ 1) << 12) | ((unsigned long)immr << 6) | (nimms & 0x3f);
    return 0;
}

static unsigned long rd(const AsmInst *in, int k) {
    return in->ops[k].reg;
}

/* the branch displacement to `sym`, which must be in this section; -1 after an error */
static int local_target(As *a, int sec, size_t off, const AsmOp *op, long *disp) {
    const AsmSym *sym = &a->syms[op->sym];
    if (sym->section != sec) {
        as_error(a, sym->section < 0 ? "undefined label %s" : "branch to %s, which is not in this section", sym->name);
        return -1;
    }
    *disp = (long)sym->off - (long)off + op->imm;
    return 0;
}

static int arm64_encode(As *a, int sec, const AsmItem *it, unsigned char *out) {
    const AsmInst *in = &it->u.insn;
    const AsmRow *row = &a->arch->rows[in->row];
    unsigned long w = row->op;
    int bits = in->ops[0].kind == OP_REG && (in->ops[0].cls == R_W || in->ops[0].cls == R_WZR) ? 32 : 64;
    long disp;
    long imm;

    switch (row->fam) {
        case F_FIXED:
            break;
        case F_RR:
            w |= rd(in, 1) << 5 | rd(in, 0);
            break;
        case F_RM:
            w |= rd(in, 1) << 16 | rd(in, 0);
            break;
        case F_RRR:
            if (in->nops == 2) {
                w |= rd(in, 1) << 16 | rd(in, 0) << 5;
            } else {
                w |= rd(in, 2) << 16 | rd(in, 1) << 5 | rd(in, 0);
            }
            break;
        case F_RRR_SHIFT:
            imm = in->ops[3].imm;
            if (imm < 0 || imm >= bits) {
                goto range;
            }
            w |= rd(in, 2) << 16 | (unsigned long)imm << 10 | rd(in, 1) << 5 | rd(in, 0);
            break;
        case F_MADD:
            w |= rd(in, 2) << 16 | rd(in, 3) << 10 | rd(in, 1) << 5 | rd(in, 0);
            break;
        case F_ADDSUB_IMM:
            if (in->nops == 2) {
                imm = in->ops[1].imm;
                w |= rd(in, 0) << 5;
            } else {
                imm = in->ops[2].imm;
                w |= rd(in, 1) << 5 | rd(in, 0);
            }
            if (imm < 0 || imm > 4095) {
                goto range;
            }
            w |= (unsigned long)imm << 10;
            break;
        case F_LOGIMM: {
            unsigned long enc;
            if (logical_imm((unsigned long)in->ops[2].imm, bits, &enc) != 0) {
                as_error(a, "%ld is not a logical immediate", in->ops[2].imm);
                return -1;
            }
            w |= enc << 10 | rd(in, 1) << 5 | rd(in, 0);
            break;
        }
        case F_MOVW:
            imm = in->ops[1].imm;
            if (imm < 0 || imm > 0xffff) {
                goto range;
            }
            if (in->nops == 3) {
                long sh = in->ops[2].imm;
                if (sh != 0 && sh != 16 && sh != 32 && sh != 48) {
                    goto range;
                }
                w |= (unsigned long)(sh / 16) << 21;
            }
            w |= (unsigned long)imm << 5 | rd(in, 0);
            break;
        case F_MOVI: {
            unsigned long v = (unsigned long)in->ops[1].imm;
            int sh;
            for (sh = 0; sh < 64; sh += 16) {
                if ((v & ~(0xfffful << sh)) == 0) {
                    w = 0xd2800000ul | (unsigned long)(sh / 16) << 21 | (v >> sh) << 5 | rd(in, 0);
                    break;
                }
                if ((~v & ~(0xfffful << sh)) == 0) {
                    w = 0x92800000ul | (unsigned long)(sh / 16) << 21 | ((~v >> sh) & 0xffff) << 5 | rd(in, 0);
                    break;
                }
            }
            if (sh == 64) {
                as_error(a, "%ld needs more than one instruction to load", in->ops[1].imm);
                return -1;
            }
            break;
        }
        case F_LSL_IMM:
            imm = in->ops[2].imm;
            if (imm < 0 || imm >= bits) {
                goto range;
            }
            w |= (unsigned long)((bits - imm) & (bits - 1)) << 16 | (unsigned long)(bits - 1 - imm) << 10 | rd(in, 1) << 5 | rd(in, 0);
            break;
        case F_SHR_IMM:
            imm = in->ops[2].imm;
            if (imm < 0 || imm >= bits) {
                goto range;
            }
            w |= (unsigned long)imm << 16 | (unsigned long)(bits - 1) << 10 | rd(in, 1) << 5 | rd(in, 0);
            break;
        case F_BFI: {
            long lsb = in->ops[2].imm;
            long width = in->ops[3].imm;
            if (lsb < 0 || lsb >= bits || width < 1 || lsb + width > bits) {
                goto range;
            }
            w |= (unsigned long)((bits - lsb) & (bits - 1)) << 16 | (unsigned long)(width - 1) << 10 | rd(in, 1) << 5 | rd(in, 0);
            break;
        }
        case F_BFX: {
            long lsb = in->ops[2].imm;
            long width = in->ops[3].imm;
            if (lsb < 0 || lsb >= bits || width < 1 || lsb + width > bits) {
                goto range;
            }
            w |= (unsigned long)lsb << 16 | (unsigned long)(lsb + width - 1) << 10 | rd(in, 1) << 5 | rd(in, 0);
            break;
        }
        case F_CSET:
            w |= (unsigned long)(in->ops[1].imm ^ 1) << 12 | rd(in, 0);
            break;
        case F_B: {
            const AsmSym *sym = &a->syms[in->ops[0].sym];
            if (sym->section == sec) {
                disp = (long)sym->off - (long)it->off;
                if (disp < -(128l << 20) || disp >= (128l << 20)) {
                    goto range;
                }
                w |= ((unsigned long)disp >> 2) & 0x3ffffff;
            } else if (sym->local) {
                as_error(a, "undefined label %s", sym->name);
                return -1;
            } else {
                as_reloc(a, sec, it->off, in->ops[0].sym, 0, R_BRANCH26);
            }
            break;
        }
        case F_BCOND:
        case F_CBZ:
            if (local_target(a, sec, it->off, &in->ops[1], &disp) != 0) {
                return -1;
            }
            if (disp < -(1l << 20) || disp >= (1l << 20)) {
                goto range;
            }
            w |= (((unsigned long)disp >> 2) & 0x7ffff) << 5;
            w |= row->fam == F_CBZ ? rd(in, 0) : (unsigned long)in->ops[0].imm;
            break;
        case F_BLR:
            w |= rd(in, 0) << 5;
            break;
        case F_BRK:
            imm = in->ops[0].imm;
            if (imm < 0 || imm > 0xffff) {
                goto range;
            }
            w |= (unsigned long)imm << 5;
            break;
        case F_FMOVI: {
            /* imm8 abcdefgh is the value a:NOT(b):bbbbbbbb:cd:efgh:0...; the double's bits must have that form */
            unsigned long d = (unsigned long)in->ops[1].imm;
            unsigned long rep = (d >> 54) & 0xff;
            unsigned long b6 = (d >> 61) & 1;
            if ((d & 0xffffffffffffUL) != 0 || (rep != 0 && rep != 0xff) || ((d >> 62) & 1) == b6 || (rep & 1) != b6) {
                goto range;
            }
            w |= (((d >> 63) & 1) << 7 | b6 << 6 | ((d >> 48) & 0x3f)) << 13 | rd(in, 0);
            break;
        }
        case F_ADR:
            if (local_target(a, sec, it->off, &in->ops[1], &disp) != 0) {
                return -1;
            }
            if (disp < -(1l << 20) || disp >= (1l << 20)) {
                goto range;
            }
            w |= ((unsigned long)disp & 3) << 29 | (((unsigned long)disp >> 2) & 0x7ffff) << 5 | rd(in, 0);
            break;
        case F_ADRP:
            w |= rd(in, 0);
            as_reloc(a, sec, it->off, in->ops[1].sym, in->ops[1].imm, in->ops[1].mode == SYM_PAGE ? R_PAGE21 : R_GOT_PAGE21);
            break;
        case F_PAGEOFF:
            w |= rd(in, 1) << 5 | rd(in, 0);
            as_reloc(a, sec, it->off, in->ops[2].sym, in->ops[2].imm, R_PAGEOFF12);
            break;
        case F_LDST: {
            const AsmOp *m = &in->ops[1];
            unsigned long scale = 1ul << (w >> 30);
            w |= (unsigned long)m->base << 5 | rd(in, 0);
            if (m->mode == MEM_SYM) {
                as_reloc(a, sec, it->off, m->sym, 0, R_GOT_PAGEOFF12);
            } else if (m->imm >= 0 && (unsigned long)m->imm % scale == 0 && (unsigned long)m->imm / scale < 4096) {
                w |= ((unsigned long)m->imm / scale) << 10;
            } else if (m->imm >= -256 && m->imm <= 255) {
                w &= ~(1ul << 24);
                w |= ((unsigned long)m->imm & 0x1ff) << 12;
            } else {
                goto range;
            }
            break;
        }
        case F_LDST_REG:
            w |= (unsigned long)in->ops[1].index << 16 | (unsigned long)in->ops[1].base << 5 | rd(in, 0);
            break;
        case F_LDP: {
            const AsmOp *m = &in->ops[2];
            if (m->imm % 8 != 0 || m->imm < -512 || m->imm > 504) {
                goto range;
            }
            if (m->mode == MEM_PRE) {
                w |= 1ul << 23;
            } else if (m->mode == MEM_POST) {
                w = (w & ~(1ul << 24)) | 1ul << 23;
            }
            w |= (((unsigned long)(m->imm / 8)) & 0x7f) << 15 | rd(in, 1) << 10 | (unsigned long)m->base << 5 | rd(in, 0);
            break;
        }
    }
    out[0] = (unsigned char)w;
    out[1] = (unsigned char)(w >> 8);
    out[2] = (unsigned char)(w >> 16);
    out[3] = (unsigned char)(w >> 24);
    return 0;
range:
    as_error(a, "operand out of range in %s", row->mnem);
    return -1;
}

/* sorted by mnemonic, then shape (strcmp order); asm.c checks */
static const AsmRow arm64_rows[] = {
    { "adc", "XXX", F_RRR, 0x9a000000 },
    { "add", "PPI", F_ADDSUB_IMM, 0x91000000 },
    { "add", "PPX", F_RRR, 0x8b206000 },
    { "add", "WWI", F_ADDSUB_IMM, 0x11000000 },
    { "add", "WWW", F_RRR, 0x0b000000 },
    { "add", "XPI", F_ADDSUB_IMM, 0x91000000 },
    { "add", "XXI", F_ADDSUB_IMM, 0x91000000 },
    { "add", "XXO", F_PAGEOFF, 0x91000000 },
    { "add", "XXX", F_RRR, 0x8b000000 },
    { "add", "XXXK", F_RRR_SHIFT, 0x8b000000 },
    { "adds", "XXX", F_RRR, 0xab000000 },
    { "adr", "XL", F_ADR, 0x10000000 },
    { "adrp", "XG", F_ADRP, 0x90000000 },
    { "adrp", "Xg", F_ADRP, 0x90000000 },
    { "and", "WWI", F_LOGIMM, 0x12000000 },
    { "and", "WWW", F_RRR, 0x0a000000 },
    { "and", "XXI", F_LOGIMM, 0x92000000 },
    { "and", "XXX", F_RRR, 0x8a000000 },
    { "asr", "WWI", F_SHR_IMM, 0x13000000 },
    { "asr", "WWW", F_RRR, 0x1ac02800 },
    { "asr", "XXI", F_SHR_IMM, 0x93400000 },
    { "asr", "XXX", F_RRR, 0x9ac02800 },
    { "b", "L", F_B, 0x14000000 },
    { "b.", "CL", F_BCOND, 0x54000000 },
    { "bfi", "XXII", F_BFI, 0xb3400000 },
    { "bl", "L", F_B, 0x94000000 },
    { "blr", "X", F_BLR, 0xd63f0000 },
    { "br", "X", F_BLR, 0xd61f0000 },
    { "brk", "I", F_BRK, 0xd4200000 },
    { "cbnz", "WL", F_CBZ, 0x35000000 },
    { "cbnz", "XL", F_CBZ, 0xb5000000 },
    { "cbz", "WL", F_CBZ, 0x34000000 },
    { "cbz", "XL", F_CBZ, 0xb4000000 },
    { "clz", "WW", F_RR, 0x5ac01000 },
    { "clz", "XX", F_RR, 0xdac01000 },
    { "cmn", "WI", F_ADDSUB_IMM, 0x3100001f },
    { "cmn", "XI", F_ADDSUB_IMM, 0xb100001f },
    { "cmp", "WI", F_ADDSUB_IMM, 0x7100001f },
    { "cmp", "WW", F_RRR, 0x6b00001f },
    { "cmp", "XI", F_ADDSUB_IMM, 0xf100001f },
    { "cmp", "XX", F_RRR, 0xeb00001f },
    { "cset", "WC", F_CSET, 0x1a9f07e0 },
    { "dmb", "B", F_FIXED, 0xd5033bbf },
    { "eor", "WWI", F_LOGIMM, 0x52000000 },
    { "eor", "WWW", F_RRR, 0x4a000000 },
    { "eor", "XXI", F_LOGIMM, 0xd2000000 },
    { "eor", "XXX", F_RRR, 0xca000000 },
    { "fabs", "DD", F_RR, 0x1e60c000 },
    { "fabs", "SS", F_RR, 0x1e20c000 },
    { "fadd", "DDD", F_RRR, 0x1e602800 },
    { "fadd", "SSS", F_RRR, 0x1e202800 },
    { "fcmp", "DD", F_RRR, 0x1e602000 },
    { "fcmp", "SS", F_RRR, 0x1e202000 },
    { "fcvt", "DS", F_RR, 0x1e22c000 },
    { "fcvt", "SD", F_RR, 0x1e624000 },
    { "fcvtzs", "WD", F_RR, 0x1e780000 },
    { "fcvtzs", "WS", F_RR, 0x1e380000 },
    { "fcvtzs", "XD", F_RR, 0x9e780000 },
    { "fcvtzs", "XS", F_RR, 0x9e380000 },
    { "fcvtzu", "WD", F_RR, 0x1e790000 },
    { "fcvtzu", "WS", F_RR, 0x1e390000 },
    { "fcvtzu", "XD", F_RR, 0x9e790000 },
    { "fcvtzu", "XS", F_RR, 0x9e390000 },
    { "fdiv", "DDD", F_RRR, 0x1e601800 },
    { "fdiv", "SSS", F_RRR, 0x1e201800 },
    { "fmadd", "DDDD", F_MADD, 0x1f400000 },
    { "fmadd", "SSSS", F_MADD, 0x1f000000 },
    { "fmov", "DD", F_RR, 0x1e604000 },
    { "fmov", "DF", F_FMOVI, 0x1e601000 },
    { "fmov", "DX", F_RR, 0x9e670000 },
    { "fmov", "DZ", F_RR, 0x9e670000 },
    { "fmov", "SF", F_FMOVI, 0x1e201000 },
    { "fmov", "SW", F_RR, 0x1e270000 },
    { "fmov", "Sz", F_RR, 0x1e270000 },
    { "fmsub", "DDDD", F_MADD, 0x1f408000 },
    { "fmsub", "SSSS", F_MADD, 0x1f008000 },
    { "fmul", "DDD", F_RRR, 0x1e600800 },
    { "fmul", "SSS", F_RRR, 0x1e200800 },
    { "fneg", "DD", F_RR, 0x1e614000 },
    { "fneg", "SS", F_RR, 0x1e214000 },
    { "fnmsub", "DDDD", F_MADD, 0x1f608000 },
    { "fnmsub", "SSSS", F_MADD, 0x1f208000 },
    { "frinta", "DD", F_RR, 0x1e664000 },
    { "frinta", "SS", F_RR, 0x1e264000 },
    { "frintm", "DD", F_RR, 0x1e654000 },
    { "frintm", "SS", F_RR, 0x1e254000 },
    { "frintp", "DD", F_RR, 0x1e64c000 },
    { "frintp", "SS", F_RR, 0x1e24c000 },
    { "frintz", "DD", F_RR, 0x1e65c000 },
    { "frintz", "SS", F_RR, 0x1e25c000 },
    { "fsqrt", "DD", F_RR, 0x1e61c000 },
    { "fsqrt", "SS", F_RR, 0x1e21c000 },
    { "fsub", "DDD", F_RRR, 0x1e603800 },
    { "fsub", "SSS", F_RRR, 0x1e203800 },
    { "ldp", "XXm", F_LDP, 0xa9400000 },
    { "ldp", "XXq", F_LDP, 0xa9400000 },
    { "ldr", "Dm", F_LDST, 0xfd400000 },
    { "ldr", "Sm", F_LDST, 0xbd400000 },
    { "ldr", "Wm", F_LDST, 0xb9400000 },
    { "ldr", "Xh", F_LDST, 0xf9400000 },
    { "ldr", "Xm", F_LDST, 0xf9400000 },
    { "ldrb", "Wm", F_LDST, 0x39400000 },
    { "ldrb", "Wr", F_LDST_REG, 0x38606800 },
    { "ldrh", "Wm", F_LDST, 0x79400000 },
    { "ldrsb", "Wm", F_LDST, 0x39c00000 },
    { "ldrsb", "Xm", F_LDST, 0x39800000 },
    { "ldrsh", "Wm", F_LDST, 0x79c00000 },
    { "ldrsh", "Xm", F_LDST, 0x79800000 },
    { "ldrsw", "Xm", F_LDST, 0xb9800000 },
    { "lsl", "WWI", F_LSL_IMM, 0x53000000 },
    { "lsl", "WWW", F_RRR, 0x1ac02000 },
    { "lsl", "XXI", F_LSL_IMM, 0xd3400000 },
    { "lsl", "XXX", F_RRR, 0x9ac02000 },
    { "lsr", "WWI", F_SHR_IMM, 0x53000000 },
    { "lsr", "WWW", F_RRR, 0x1ac02400 },
    { "lsr", "XXI", F_SHR_IMM, 0xd3400000 },
    { "lsr", "XXX", F_RRR, 0x9ac02400 },
    { "madd", "WWWW", F_MADD, 0x1b000000 },
    { "madd", "XXXX", F_MADD, 0x9b000000 },
    { "mov", "PX", F_RR, 0x91000000 },
    { "mov", "WW", F_RM, 0x2a0003e0 },
    { "mov", "XI", F_MOVI, 0x00000000 },
    { "mov", "XP", F_RR, 0x91000000 },
    { "mov", "XX", F_RM, 0xaa0003e0 },
    { "movk", "XIK", F_MOVW, 0xf2800000 },
    { "movn", "XI", F_MOVW, 0x92800000 },
    { "movn", "XIK", F_MOVW, 0x92800000 },
    { "movz", "XIK", F_MOVW, 0xd2800000 },
    { "msub", "WWWW", F_MADD, 0x1b008000 },
    { "msub", "XXXX", F_MADD, 0x9b008000 },
    { "mul", "WWW", F_RRR, 0x1b007c00 },
    { "mul", "XXX", F_RRR, 0x9b007c00 },
    { "mvn", "WW", F_RM, 0x2a2003e0 },
    { "mvn", "XX", F_RM, 0xaa2003e0 },
    { "neg", "WW", F_RM, 0x4b0003e0 },
    { "neg", "XX", F_RM, 0xcb0003e0 },
    { "negs", "XX", F_RM, 0xeb0003e0 },
    { "ngc", "XX", F_RM, 0xda0003e0 },
    { "orr", "WWI", F_LOGIMM, 0x32000000 },
    { "orr", "WWW", F_RRR, 0x2a000000 },
    { "orr", "XXI", F_LOGIMM, 0xb2000000 },
    { "orr", "XXX", F_RRR, 0xaa000000 },
    { "orr", "XXXK", F_RRR_SHIFT, 0xaa000000 },
    { "rbit", "WW", F_RR, 0x5ac00000 },
    { "rbit", "XX", F_RR, 0xdac00000 },
    { "ret", "", F_FIXED, 0xd65f03c0 },
    { "rev", "WW", F_RR, 0x5ac00800 },
    { "rev", "XX", F_RR, 0xdac00c00 },
    { "rev16", "WW", F_RR, 0x5ac00400 },
    { "sbc", "XXX", F_RRR, 0xda000000 },
    { "sbcs", "ZXX", F_RRR, 0xfa000000 },
    { "sbfx", "XXII", F_BFX, 0x93400000 },
    { "scvtf", "DW", F_RR, 0x1e620000 },
    { "scvtf", "DX", F_RR, 0x9e620000 },
    { "scvtf", "SW", F_RR, 0x1e220000 },
    { "scvtf", "SX", F_RR, 0x9e220000 },
    { "sdiv", "WWW", F_RRR, 0x1ac00c00 },
    { "sdiv", "XXX", F_RRR, 0x9ac00c00 },
    { "stp", "XXm", F_LDP, 0xa9000000 },
    { "stp", "XXp", F_LDP, 0xa9000000 },
    { "str", "Dm", F_LDST, 0xfd000000 },
    { "str", "Sm", F_LDST, 0xbd000000 },
    { "str", "Wm", F_LDST, 0xb9000000 },
    { "str", "Xm", F_LDST, 0xf9000000 },
    { "str", "Zm", F_LDST, 0xf9000000 },
    { "str", "zm", F_LDST, 0xb9000000 },
    { "strb", "Wm", F_LDST, 0x39000000 },
    { "strb", "Wr", F_LDST_REG, 0x38206800 },
    { "strb", "zm", F_LDST, 0x39000000 },
    { "strb", "zr", F_LDST_REG, 0x38206800 },
    { "strh", "Wm", F_LDST, 0x79000000 },
    { "strh", "zm", F_LDST, 0x79000000 },
    { "sub", "PPI", F_ADDSUB_IMM, 0xd1000000 },
    { "sub", "PPX", F_RRR, 0xcb206000 },
    { "sub", "WWI", F_ADDSUB_IMM, 0x51000000 },
    { "sub", "WWW", F_RRR, 0x4b000000 },
    { "sub", "XXI", F_ADDSUB_IMM, 0xd1000000 },
    { "sub", "XXX", F_RRR, 0xcb000000 },
    { "subs", "XXX", F_RRR, 0xeb000000 },
    { "sxtb", "WW", F_RR, 0x13001c00 },
    { "sxtb", "XW", F_RR, 0x93401c00 },
    { "sxth", "WW", F_RR, 0x13003c00 },
    { "sxth", "XW", F_RR, 0x93403c00 },
    { "sxtw", "XW", F_RR, 0x93407c00 },
    { "ubfx", "XXII", F_BFX, 0xd3400000 },
    { "ucvtf", "DW", F_RR, 0x1e630000 },
    { "ucvtf", "DX", F_RR, 0x9e630000 },
    { "ucvtf", "SW", F_RR, 0x1e230000 },
    { "ucvtf", "SX", F_RR, 0x9e230000 },
    { "udiv", "WWW", F_RRR, 0x1ac00800 },
    { "udiv", "XXX", F_RRR, 0x9ac00800 },
    { "umulh", "XXX", F_RRR, 0x9bc07c00 },
    { "uxtb", "WW", F_RR, 0x53001c00 },
    { "uxth", "WW", F_RR, 0x53003c00 },
};

const AsmArch as_arm64 = { "arm64", arm64_rows, sizeof(arm64_rows) / sizeof(arm64_rows[0]), 0, SECSYM_LTMP, arm64_parse, arm64_shape, arm64_insn_size, arm64_encode, arm64_pad, macho_write };
