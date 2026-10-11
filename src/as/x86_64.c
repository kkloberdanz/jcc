/*
 * The x86_64 half of the assembler: AT&T syntax as the x86_64 backend
 * writes it, and the table of instruction forms it emits (tools/as-corpus.sh
 * collects the corpus the table was read off). The operand alphabet of
 * the table's shapes, in AT&T operand order (source first):
 *
 *   q  a 64 bit register    l  32 bit    w  16 bit    b  8 bit    x  %xmmN    t  %st or %st(i)
 *   I  $imm
 *   L  a label or symbol    P  sym@PLT   J  *%reg (an indirect call)
 *   m  disp(%base) or (%base)            i  (%base, %index[, scale])
 *   R  sym(%rip)            G  sym@GOTPCREL(%rip)
 *
 * The statics both halves define (the AsmArch entries and the operand
 * parser) carry the x86_64_ prefix here and arm64_ in arm64.c, so that
 * both can live in one translation unit, the amalgamation.
 */
#include <stdlib.h>
#include <string.h>

#include "asm.h"

enum { R_64, R_32, R_16, R_8, R_8H, R_XMM, R_ST };

/* the name less its %, in encoding order within each class */
static const char *const names64[16] = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
static const char *const names32[16] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d" };
static const char *const names16[16] = { "ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w" };
static const char *const names8[16] = { "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil", "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b" };
static const char *const names8h[4] = { "ah", "ch", "dh", "bh" };

static int in_table(const char *const *names, int n, const char *s, size_t len) {
    int i;
    for (i = 0; i < n; i++) {
        if (strlen(names[i]) == len && memcmp(names[i], s, len) == 0) {
            return i;
        }
    }
    return -1;
}

static int x86_64_is_punct(const AsmTok *t, char c) {
    return t->kind == ATOK_PUNCT && t->punct == c;
}

/* %name at t[*i] (and %st(i)): 0 with the class and number, or -1 */
static int parse_reg(const AsmTok *t, size_t *i, int *cls, int *num) {
    const AsmTok *name = &t[*i + 1];
    int n;
    if (!x86_64_is_punct(&t[*i], '%') || name->kind != ATOK_IDENT) {
        return -1;
    }
    if ((n = in_table(names64, 16, name->s, name->len)) >= 0) *cls = R_64;
    else if ((n = in_table(names32, 16, name->s, name->len)) >= 0) *cls = R_32;
    else if ((n = in_table(names16, 16, name->s, name->len)) >= 0) *cls = R_16;
    else if ((n = in_table(names8, 16, name->s, name->len)) >= 0) *cls = R_8;
    else if ((n = in_table(names8h, 4, name->s, name->len)) >= 0) { *cls = R_8H; n += 4; }
    else if (name->len == 2 && memcmp(name->s, "st", 2) == 0) {
        *cls = R_ST;
        n = 0;
        if (x86_64_is_punct(&t[*i + 2], '(')) {
            if (t[*i + 3].kind != ATOK_NUM || t[*i + 3].num > 7 || !x86_64_is_punct(&t[*i + 4], ')')) {
                return -1;
            }
            n = (int)t[*i + 3].num;
            *i += 3;
        }
    } else if (name->len > 3 && memcmp(name->s, "xmm", 3) == 0) {
        size_t k;
        n = 0;
        for (k = 3; k < name->len; k++) {
            if (name->s[k] < '0' || name->s[k] > '9') {
                return -1;
            }
            n = n * 10 + (name->s[k] - '0');
        }
        if (n > 15 || (name->len > 4 && name->s[3] == '0')) {
            return -1;
        }
        *cls = R_XMM;
    } else {
        return -1;
    }
    *num = n;
    *i += 2;
    return 0;
}

/* [-]n at t[*i] */
static int parse_num(const AsmTok *t, size_t *i, long *v) {
    int neg = 0;
    if (x86_64_is_punct(&t[*i], '-')) {
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

/* (%base[, %index[, scale]]) or (%rip) at t[*i], into a memory operand whose displacement is set already */
static int parse_mem(const AsmTok *t, size_t *i, AsmOp *op) {
    int cls;
    int num;
    if (!x86_64_is_punct(&t[*i], '(')) {
        return -1;
    }
    (*i)++;
    if (x86_64_is_punct(&t[*i], '%') && as_tok_is(&t[*i + 1], "rip")) {
        if (op->mode != MEM_SYM || !x86_64_is_punct(&t[*i + 2], ')')) {
            return -1;
        }
        *i += 3;
        return 0;
    }
    if (op->mode == MEM_SYM) {
        return -1;   /* a symbol displacement is only rip-relative */
    }
    if (parse_reg(t, i, &cls, &num) != 0 || cls != R_64) {
        return -1;
    }
    op->base = (unsigned char)num;
    op->scale = 1;
    if (x86_64_is_punct(&t[*i], ',')) {
        (*i)++;
        if (parse_reg(t, i, &cls, &num) != 0 || cls != R_64) {
            return -1;
        }
        op->mode = MEM_INDEX;
        op->index = (unsigned char)num;
        if (x86_64_is_punct(&t[*i], ',')) {
            (*i)++;
            if (t[*i].kind != ATOK_NUM || (t[*i].num != 1 && t[*i].num != 2 && t[*i].num != 4 && t[*i].num != 8)) {
                return -1;
            }
            op->scale = (unsigned char)t[*i].num;
            (*i)++;
        }
    }
    if (!x86_64_is_punct(&t[*i], ')')) {
        return -1;
    }
    (*i)++;
    return 0;
}

static int x86_64_parse_operand(As *a, const AsmTok *t, size_t *i, AsmOp *op) {
    int cls;
    int num;
    long v = 0;

    memset(op, 0, sizeof(*op));
    op->base = op->index = NO_REG;
    op->sym = -1;
    if (parse_reg(t, i, &cls, &num) == 0) {
        op->kind = OP_REG;
        op->cls = (unsigned char)cls;
        op->reg = (unsigned char)num;
        return 0;
    }
    if (x86_64_is_punct(&t[*i], '$')) {
        (*i)++;
        if (parse_num(t, i, &v) != 0) {
            return -1;
        }
        op->kind = OP_IMM;
        op->imm = v;
        return 0;
    }
    if (x86_64_is_punct(&t[*i], '*')) {
        (*i)++;
        if (parse_reg(t, i, &cls, &num) != 0) {
            return -1;
        }
        op->kind = OP_MEM;
        op->mode = MEM_INDIRECT;
        op->cls = (unsigned char)cls;
        op->reg = (unsigned char)num;
        return 0;
    }
    if (x86_64_is_punct(&t[*i], '(') || x86_64_is_punct(&t[*i], '-') || t[*i].kind == ATOK_NUM) {
        op->kind = OP_MEM;
        op->mode = MEM_BASE;
        if (!x86_64_is_punct(&t[*i], '(') && parse_num(t, i, &v) != 0) {
            return -1;
        }
        op->imm = v;
        return parse_mem(t, i, op);
    }
    if (t[*i].kind == ATOK_IDENT) {
        int sym = as_symbol(a, t[*i].s, t[*i].len);
        int mode = SYM_PLAIN;
        (*i)++;
        if (x86_64_is_punct(&t[*i], '@')) {
            if (as_tok_is(&t[*i + 1], "PLT")) mode = SYM_PLT;
            else if (as_tok_is(&t[*i + 1], "GOTPCREL")) mode = SYM_GOTPCREL;
            else return -1;
            *i += 2;
        }
        if (x86_64_is_punct(&t[*i], '(') || mode == SYM_GOTPCREL) {
            op->kind = OP_MEM;
            op->mode = MEM_SYM;
            op->sym = sym;
            op->cls = (unsigned char)mode;    /* SYM_PLAIN or SYM_GOTPCREL */
            return parse_mem(t, i, op);
        }
        op->kind = OP_SYM;
        op->mode = (unsigned char)mode;
        op->sym = sym;
        return 0;
    }
    return -1;
}

static int x86_64_parse(As *a, char *mnem, const AsmTok *t, size_t n, AsmInst *out) {
    size_t i = 0;
    out->nops = 0;
    while (i < n) {
        if (out->nops == MAX_OPS) {
            as_error(a, "too many operands");
            return -1;
        }
        if (x86_64_parse_operand(a, t, &i, &out->ops[out->nops]) != 0) {
            as_error(a, "cannot parse operand %d of %s", out->nops + 1, mnem);
            return -1;
        }
        out->nops++;
        if (i < n) {
            if (!x86_64_is_punct(&t[i], ',')) {
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

static void x86_64_shape(const AsmInst *in, char *buf) {
    size_t k;
    for (k = 0; k < in->nops; k++) {
        const AsmOp *op = &in->ops[k];
        char c = '?';
        switch (op->kind) {
            case OP_REG:
                switch (op->cls) {
                    case R_64: c = 'q'; break;
                    case R_32: c = 'l'; break;
                    case R_16: c = 'w'; break;
                    case R_8: c = 'b'; break;
                    case R_8H: c = 'h'; break;
                    case R_XMM: c = 'x'; break;
                    case R_ST: c = 't'; break;
                }
                break;
            case OP_IMM: c = 'I'; break;
            case OP_SYM: c = op->mode == SYM_PLAIN ? 'L' : op->mode == SYM_PLT ? 'P' : '?'; break;
            case OP_MEM:
                switch (op->mode) {
                    case MEM_BASE: c = 'm'; break;
                    case MEM_INDEX: c = 'i'; break;
                    case MEM_SYM: c = op->cls == SYM_GOTPCREL ? 'G' : 'R'; break;
                    case MEM_INDIRECT: c = op->cls == R_64 ? 'J' : '?'; break;
                }
                break;
        }
        buf[k] = c;
    }
    buf[k] = '\0';
}

/* ------------------------------------------------------------------ */
/* encoding                                                            */
/* ------------------------------------------------------------------ */

/*
 * A row's `op` packs the opcode (its bytes, 0f escapes included, in the
 * low 24 bits), the /digit of the ModRM byte, a legacy prefix and REX.W;
 * the family says what the operands fill in. The reference is `cc -c`
 * on the corpus, which tests/as-obj.sh compares against byte for byte.
 */
#define DIG(n) ((unsigned long)(n) << 24)
#define P66 (1ul << 28)
#define PF2 (2ul << 28)
#define PF3 (3ul << 28)
#define REXW (1ul << 30)

enum {
    X_FIXED,        /* the opcode bytes alone */
    X_RM,           /* opcode /r: reg is the last operand, r/m the one before it */
    X_MR,           /* opcode /r: reg is the operand before the last, r/m the last */
    X_M,            /* opcode /digit: r/m is the last operand (the only one, or the one after %cl) */
    X_M_IB,         /* opcode /digit ib: $imm8, r/m (btcq) */
    X_O,            /* opcode + register number (pushq, bswap) */
    X_ALU_I,        /* $imm, r/m: 83 /digit ib, 81 /digit id, or the accumulator's short form digit*8+5 id */
    X_MOV_I,        /* $imm, r: b8+r id; $imm, r/m with REX.W or in memory: c7 /0 id */
    X_MOVABS,       /* $imm64, r: b8+r io */
    X_TEST_IB,      /* $imm8, r/m8: a8 ib for %al, f6 /0 ib otherwise */
    X_SHIFT_I,      /* $imm, r/m: d1 /digit for a shift by one, c1 /digit ib otherwise */
    X_IMUL3,        /* $imm, r/m, reg: 6b /r ib or 69 /r id */
    X_JMP,          /* eb rel8 or e9 rel32 */
    X_JCC,          /* 7x rel8 or 0f 8x rel32: op is the condition x */
    X_CALL,         /* e8 rel32 with a PLT32 relocation */
    X_ST,           /* an x87 register form: the second opcode byte + i of %st(i), the last operand */
    X_ST0           /* the same with %st(i) the first operand (fucomip, fstp) */
};

typedef struct Enc {
    unsigned char b[16];
    size_t n;
} Enc;

static void x86_64_put(Enc *e, unsigned long v, int bytes) {
    int i;
    for (i = 0; i < bytes; i++) {
        e->b[e->n++] = (unsigned char)(v >> (8 * i));
    }
}

/* the operand size of a row whose immediate depends on it, from the mnemonic's suffix */
static int op_bits(const AsmRow *row) {
    switch (row->mnem[strlen(row->mnem) - 1]) {
        case 'q': return 64;
        case 'l': return 32;
        case 'w': return 16;
        default: return 8;
    }
}

/*
 * An immediate for a `bits`-wide operand, as the sign-extended value the
 * instruction encodes: a 32-bit operand takes anything that fits in 32
 * bits either way (clang reads $4294967295 as -1), a 64-bit one only a
 * signed 32-bit value. -1 for anything else.
 */
static int imm_value(long v, int bits, long *out) {
    if (bits == 32) {
        unsigned long u;
        if (v < -(1l << 31) || v > (1l << 32) - 1) {
            return -1;
        }
        u = (unsigned long)v & 0xfffffffful;
        *out = u >= 0x80000000ul ? (long)u - (1l << 32) : (long)u;
        return 0;
    }
    if (v < -(1l << 31) || v > (1l << 31) - 1) {
        return -1;
    }
    *out = v;
    return 0;
}

static int fits8(long v) {
    return v >= -128 && v <= 127;
}

/* %spl, %bpl, %sil, %dil: byte registers only reachable with a REX prefix */
static int wants_rex(const AsmOp *op) {
    return op->kind == OP_REG && op->cls == R_8 && op->reg >= 4 && op->reg < 8;
}

static int in_rm_reg(const AsmOp *op) {
    return op->kind == OP_REG || op->mode == MEM_INDIRECT;
}

/*
 * The ModRM byte for `rm` with `regfield` in its reg bits, and the SIB
 * and displacement after it; `trailing` immediate bytes follow. A
 * rip-relative operand naming a local symbol in this section is resolved
 * here, any other gets a PC32 (or GOTPCRELX) relocation.
 */
static int modrm(As *a, int sec, const AsmItem *it, Enc *e, unsigned regfield, const AsmOp *rm, int rex, size_t trailing, int dry) {
    unsigned char mod;
    long disp = rm->imm;
    unsigned base = rm->base;

    regfield = (regfield & 7) << 3;
    if (in_rm_reg(rm)) {
        e->b[e->n++] = (unsigned char)(0xc0 | regfield | (rm->reg & 7));
        return 0;
    }
    if (rm->mode == MEM_SYM) {
        const AsmSym *sym = &a->syms[rm->sym];
        size_t field;
        e->b[e->n++] = (unsigned char)(regfield | 5);
        field = e->n;
        if (rm->cls == SYM_PLAIN && sym->section == sec && !sym->global) {
            disp = (long)sym->off + rm->imm - (long)(it->off + field + 4 + trailing);
        } else if (sym->local && sym->section < 0) {
            if (!dry) {
                as_error(a, "undefined label %s", sym->name);
            }
            return -1;
        } else {
            if (!dry) {
                int kind = rm->cls == SYM_GOTPCREL ? (rex ? R_REX_GOTPCRELX : R_GOTPCRELX) : R_PC32;
                as_reloc(a, sec, it->off + field, rm->sym, rm->imm - 4 - (long)trailing, kind);
            }
            disp = 0;
        }
        x86_64_put(e, (unsigned long)disp, 4);
        return 0;
    }
    if (disp < -(1l << 31) || disp > (1l << 31) - 1) {
        if (!dry) {
            as_error(a, "displacement %ld is out of range", disp);
        }
        return -1;
    }
    if (disp == 0 && (base & 7) != 5) {
        mod = 0;
    } else if (fits8(disp)) {
        mod = 1;
    } else {
        mod = 2;
    }
    if (rm->mode == MEM_INDEX) {
        unsigned scale = rm->scale == 8 ? 3 : rm->scale == 4 ? 2 : rm->scale == 2 ? 1 : 0;
        if (rm->index == 4) {
            if (!dry) {
                as_error(a, "%%rsp cannot be an index register");
            }
            return -1;
        }
        e->b[e->n++] = (unsigned char)(mod << 6 | regfield | 4);
        e->b[e->n++] = (unsigned char)(scale << 6 | (rm->index & 7) << 3 | (base & 7));
    } else if ((base & 7) == 4) {
        e->b[e->n++] = (unsigned char)(mod << 6 | regfield | 4);
        e->b[e->n++] = 0x24;
    } else {
        e->b[e->n++] = (unsigned char)(mod << 6 | regfield | (base & 7));
    }
    if (mod == 1) {
        x86_64_put(e, (unsigned long)disp, 1);
    } else if (mod == 2) {
        x86_64_put(e, (unsigned long)disp, 4);
    }
    return 0;
}

/*
 * jmp, jcc and call. A jump goes to a label in this section and starts
 * out short (rel8); the layout passes grow it to rel32 once the distance
 * does not fit, and never back (it->size remembers), as clang's
 * relaxation does. A call carries a PLT32 relocation.
 */
static size_t x86_64_branch(As *a, int sec, const AsmItem *it, Enc *e, int dry) {
    const AsmInst *in = &it->u.insn;
    const AsmRow *row = &a->arch->rows[in->row];
    const AsmOp *t = &in->ops[0];
    const AsmSym *sym = &a->syms[t->sym];
    size_t longlen = row->fam == X_JMP ? 5 : 6;
    long disp;

    if (row->fam == X_CALL) {
        if (sym->local && sym->section < 0) {
            if (!dry) {
                as_error(a, "undefined label %s", sym->name);
            }
            return 0;
        }
        e->b[e->n++] = 0xe8;
        if (!dry) {
            as_reloc(a, sec, it->off + 1, t->sym, t->imm - 4, R_PLT32);
        }
        x86_64_put(e, 0, 4);
        return e->n;
    }
    if (sym->section != sec) {
        if (!dry) {
            as_error(a, sym->section < 0 ? "undefined label %s" : "jump to %s, which is not in this section", sym->name);
        }
        return 0;
    }
    disp = (long)sym->off + t->imm - (long)(it->off + 2);
    if (it->size != longlen && (it->size == 0 || fits8(disp))) {
        /* short: the first layout pass assumes every jump is */
        e->b[e->n++] = (unsigned char)(row->fam == X_JMP ? 0xeb : 0x70 | row->op);
        x86_64_put(e, (unsigned long)disp, 1);
        return e->n;
    }
    disp = (long)sym->off + t->imm - (long)(it->off + longlen);
    if (row->fam == X_JMP) {
        e->b[e->n++] = 0xe9;
    } else {
        e->b[e->n++] = 0x0f;
        e->b[e->n++] = (unsigned char)(0x80 | row->op);
    }
    x86_64_put(e, (unsigned long)disp, 4);
    return e->n;
}

/*
 * Encode an instruction into `e`. In a dry run (layout) nothing is
 * reported or recorded and only the length matters; the final pass
 * reports errors and records relocations. Returns the length, 0 after
 * an error.
 */
static size_t gen(As *a, int sec, const AsmItem *it, Enc *e, int dry) {
    const AsmInst *in = &it->u.insn;
    const AsmRow *row = &a->arch->rows[in->row];
    unsigned long op = row->op;
    unsigned long opcode = op & 0xffffff;
    unsigned digit = (unsigned)(op >> 24) & 7;
    unsigned prefix = (unsigned)(op >> 28) & 3;
    unsigned rex = (op & REXW) ? 0x48 : 0x40;
    int need_rex = 0;
    const AsmOp *reg = NULL;        /* the operand in the reg field, or NULL for /digit */
    const AsmOp *rm = NULL;         /* the operand in the r/m field, or NULL for no ModRM */
    int opreg = -1;                 /* the register added to the opcode */
    long imm = 0;
    int immsize = 0;
    int bits = op_bits(row);
    int k;
    int started = 0;

    e->n = 0;
    switch (row->fam) {
        case X_FIXED:
            break;
        case X_RM:
            reg = &in->ops[in->nops - 1];
            rm = &in->ops[in->nops - 2];
            break;
        case X_MR:
            reg = &in->ops[in->nops - 2];
            rm = &in->ops[in->nops - 1];
            break;
        case X_M:
            rm = &in->ops[in->nops - 1];
            break;
        case X_M_IB:
            rm = &in->ops[1];
            imm = in->ops[0].imm;
            if (imm < 0 || imm >= bits) {
                goto range;
            }
            immsize = 1;
            break;
        case X_O:
            opreg = in->ops[0].reg;
            break;
        case X_ALU_I:
            rm = &in->ops[1];
            if (imm_value(in->ops[0].imm, bits, &imm) != 0) {
                goto range;
            }
            if (fits8(imm)) {
                opcode = 0x83;
                immsize = 1;
            } else if (rm->kind == OP_REG && rm->reg == 0) {
                opcode = digit * 8 + 5;
                rm = NULL;
                immsize = 4;
            } else {
                opcode = 0x81;
                immsize = 4;
            }
            break;
        case X_MOV_I:
            rm = &in->ops[1];
            if (imm_value(in->ops[0].imm, bits, &imm) != 0) {
                goto range;
            }
            immsize = 4;
            if ((op & REXW) || rm->kind != OP_REG) {
                opcode = 0xc7;
                digit = 0;
            } else {
                opcode = 0xb8;
                opreg = rm->reg;
                rm = NULL;
            }
            break;
        case X_MOVABS:
            opreg = in->ops[1].reg;
            imm = in->ops[0].imm;
            immsize = 8;
            break;
        case X_TEST_IB:
            rm = &in->ops[1];
            imm = in->ops[0].imm;
            if (imm < -128 || imm > 255) {
                goto range;
            }
            immsize = 1;
            if (rm->reg == 0) {
                opcode = 0xa8;
                rm = NULL;
            } else {
                opcode = 0xf6;
                digit = 0;
            }
            break;
        case X_SHIFT_I:
            rm = &in->ops[1];
            imm = in->ops[0].imm;
            if (imm < 0 || imm >= bits) {
                goto range;
            }
            if (imm == 1) {
                opcode = 0xd1;
            } else {
                opcode = 0xc1;
                immsize = 1;
            }
            break;
        case X_IMUL3:
            rm = &in->ops[1];
            reg = &in->ops[2];
            if (imm_value(in->ops[0].imm, bits, &imm) != 0) {
                goto range;
            }
            if (fits8(imm)) {
                opcode = 0x6b;
                immsize = 1;
            } else {
                opcode = 0x69;
                immsize = 4;
            }
            break;
        case X_ST:
            opcode += in->ops[in->nops - 1].reg;
            break;
        case X_ST0:
            opcode += in->ops[0].reg;
            break;
        case X_JMP:
        case X_JCC:
        case X_CALL:
            return x86_64_branch(a, sec, it, e, dry);
    }

    /* the REX prefix: W from the row, R, X and B from the register numbers */
    if (reg) {
        if (reg->reg >= 8) {
            rex |= 4;
        }
        need_rex |= wants_rex(reg);
    }
    if (rm) {
        if (in_rm_reg(rm)) {
            if (rm->reg >= 8) {
                rex |= 1;
            }
            need_rex |= wants_rex(rm);
        } else {
            if (rm->base != NO_REG && rm->base >= 8) {
                rex |= 1;
            }
            if (rm->mode == MEM_INDEX && rm->index >= 8) {
                rex |= 2;
            }
        }
    }
    if (opreg >= 8) {
        rex |= 1;
    }

    if (prefix) {
        e->b[e->n++] = prefix == 1 ? 0x66 : prefix == 2 ? 0xf2 : 0xf3;
    }
    if (rex != 0x40 || need_rex) {
        e->b[e->n++] = (unsigned char)rex;
    } else {
        rex = 0;
    }
    for (k = 2; k >= 0; k--) {
        unsigned byte = (unsigned)(opcode >> (8 * k)) & 0xff;
        if (byte || started || k == 0) {
            if (k == 0 && opreg >= 0) {
                byte |= (unsigned)opreg & 7;
            }
            e->b[e->n++] = (unsigned char)byte;
            started = 1;
        }
    }
    if (rm) {
        if (modrm(a, sec, it, e, reg ? reg->reg : digit, rm, rex != 0, (size_t)immsize, dry) != 0) {
            return 0;
        }
    }
    x86_64_put(e, (unsigned long)imm, immsize);
    return e->n;
range:
    if (!dry) {
        as_error(a, "operand out of range in %s", row->mnem);
    }
    return 0;
}

static size_t x86_64_insn_size(As *a, int sec, const AsmItem *it) {
    Enc e;
    return gen(a, sec, it, &e, 1);
}

static int x86_64_encode(As *a, int sec, const AsmItem *it, unsigned char *out) {
    Enc e;
    size_t n = gen(a, sec, it, &e, 0);
    if (n == 0) {
        return -1;
    }
    if (n != it->size) {
        as_error(a, "internal error: %s was laid out as %lu bytes but encodes to %lu", a->arch->rows[it->u.insn.row].mnem, (unsigned long)it->size, (unsigned long)n);
        return -1;
    }
    memcpy(out, e.b, n);
    return 0;
}

/* alignment padding: the longest nops first (clang's choice of forms, up to ten bytes each), then one for the rest */
static void x86_64_pad(unsigned char *out, size_t n) {
    static const unsigned char nops[10][10] = {
        { 0x90 },
        { 0x66, 0x90 },
        { 0x0f, 0x1f, 0x00 },
        { 0x0f, 0x1f, 0x40, 0x00 },
        { 0x0f, 0x1f, 0x44, 0x00, 0x00 },
        { 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00 },
        { 0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00 },
        { 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 }
    };
    while (n > 0) {
        size_t k = n > 10 ? 10 : n;
        memcpy(out, nops[k - 1], k);
        out += k;
        n -= k;
    }
}

/* sorted by mnemonic, then shape (strcmp order); asm.c checks */
static const AsmRow x86_64_rows[] = {
    { "adcq", "Iq", X_ALU_I, REXW | DIG(2) },
    { "adcq", "qq", X_MR, REXW | 0x11 },
    { "addl", "Il", X_ALU_I, DIG(0) },
    { "addl", "ll", X_MR, 0x01 },
    { "addq", "Iq", X_ALU_I, REXW | DIG(0) },
    { "addq", "qq", X_MR, REXW | 0x01 },
    { "addsd", "xx", X_RM, PF2 | 0x0f58 },
    { "addss", "xx", X_RM, PF3 | 0x0f58 },
    { "andb", "bb", X_MR, 0x20 },
    { "andl", "Il", X_ALU_I, DIG(4) },
    { "andl", "ll", X_MR, 0x21 },
    { "andq", "Iq", X_ALU_I, REXW | DIG(4) },
    { "andq", "qq", X_MR, REXW | 0x21 },
    { "bsfl", "ll", X_RM, 0x0fbc },
    { "bsfq", "qq", X_RM, REXW | 0x0fbc },
    { "bsrl", "ll", X_RM, 0x0fbd },
    { "bsrq", "qq", X_RM, REXW | 0x0fbd },
    { "bswapl", "l", X_O, 0x0fc8 },
    { "bswapq", "q", X_O, REXW | 0x0fc8 },
    { "btcq", "Iq", X_M_IB, REXW | DIG(7) | 0x0fba },
    { "btrq", "Iq", X_M_IB, REXW | DIG(6) | 0x0fba },
    { "call", "J", X_M, DIG(2) | 0xff },
    { "call", "P", X_CALL, 0xe8 },
    { "cltd", "", X_FIXED, 0x99 },
    { "cmpl", "Il", X_ALU_I, DIG(7) },
    { "cmpl", "ll", X_MR, 0x39 },
    { "cmpq", "Iq", X_ALU_I, REXW | DIG(7) },
    { "cmpq", "qq", X_MR, REXW | 0x39 },
    { "cqto", "", X_FIXED, REXW | 0x99 },
    { "cvtsd2ss", "xx", X_RM, PF2 | 0x0f5a },
    { "cvtsi2sdl", "lx", X_RM, PF2 | 0x0f2a },
    { "cvtsi2sdq", "qx", X_RM, PF2 | REXW | 0x0f2a },
    { "cvtsi2ssl", "lx", X_RM, PF3 | 0x0f2a },
    { "cvtsi2ssq", "qx", X_RM, PF3 | REXW | 0x0f2a },
    { "cvtss2sd", "xx", X_RM, PF3 | 0x0f5a },
    { "cvttsd2sil", "xl", X_RM, PF2 | 0x0f2c },
    { "cvttsd2siq", "xq", X_RM, PF2 | REXW | 0x0f2c },
    { "cvttss2sil", "xl", X_RM, PF3 | 0x0f2c },
    { "cvttss2siq", "xq", X_RM, PF3 | REXW | 0x0f2c },
    { "divl", "l", X_M, DIG(6) | 0xf7 },
    { "divq", "q", X_M, REXW | DIG(6) | 0xf7 },
    { "divsd", "xx", X_RM, PF2 | 0x0f5e },
    { "divss", "xx", X_RM, PF3 | 0x0f5e },
    { "faddp", "tt", X_ST, 0xdec0 },
    { "fadds", "m", X_M, DIG(0) | 0xd8 },
    { "fchs", "", X_FIXED, 0xd9e0 },
    { "fdivp", "tt", X_ST, 0xdef0 },
    { "fildq", "m", X_M, DIG(5) | 0xdf },
    { "fisttpq", "m", X_M, DIG(1) | 0xdd },
    { "fldl", "m", X_M, DIG(0) | 0xdd },
    { "flds", "m", X_M, DIG(0) | 0xd9 },
    { "fldt", "m", X_M, DIG(5) | 0xdb },
    { "fmulp", "tt", X_ST, 0xdec8 },
    { "fstp", "t", X_ST0, 0xddd8 },
    { "fstpl", "m", X_M, DIG(3) | 0xdd },
    { "fstps", "m", X_M, DIG(3) | 0xd9 },
    { "fstpt", "m", X_M, DIG(7) | 0xdb },
    { "fsubp", "tt", X_ST, 0xdee0 },
    { "fsubs", "m", X_M, DIG(4) | 0xd8 },
    { "fucomip", "tt", X_ST0, 0xdfe8 },
    { "idivl", "l", X_M, DIG(7) | 0xf7 },
    { "idivq", "q", X_M, REXW | DIG(7) | 0xf7 },
    { "imull", "Ill", X_IMUL3, 0 },
    { "imull", "ll", X_RM, 0x0faf },
    { "imulq", "Iqq", X_IMUL3, REXW },
    { "imulq", "qq", X_RM, REXW | 0x0faf },
    { "ja", "L", X_JCC, 0x7 },
    { "jae", "L", X_JCC, 0x3 },
    { "jb", "L", X_JCC, 0x2 },
    { "jbe", "L", X_JCC, 0x6 },
    { "je", "L", X_JCC, 0x4 },
    { "jg", "L", X_JCC, 0xf },
    { "jge", "L", X_JCC, 0xd },
    { "jl", "L", X_JCC, 0xc },
    { "jle", "L", X_JCC, 0xe },
    { "jmp", "J", X_M, DIG(4) | 0xff },
    { "jmp", "L", X_JMP, 0 },
    { "jne", "L", X_JCC, 0x5 },
    { "jns", "L", X_JCC, 0x9 },
    { "js", "L", X_JCC, 0x8 },
    { "leaq", "Rq", X_RM, REXW | 0x8d },
    { "leaq", "mq", X_RM, REXW | 0x8d },
    { "leave", "", X_FIXED, 0xc9 },
    { "mfence", "", X_FIXED, 0x0faef0 },
    { "movabsq", "Iq", X_MOVABS, REXW | 0xb8 },
    { "movaps", "xm", X_MR, 0x0f29 },
    { "movaps", "xx", X_RM, 0x0f28 },
    { "movb", "bm", X_MR, 0x88 },
    { "movd", "lx", X_RM, P66 | 0x0f6e },
    { "movd", "mx", X_RM, P66 | 0x0f6e },
    { "movd", "xl", X_MR, P66 | 0x0f7e },
    { "movl", "Il", X_MOV_I, 0 },
    { "movl", "Im", X_MOV_I, 0 },
    { "movl", "ll", X_MR, 0x89 },
    { "movl", "lm", X_MR, 0x89 },
    { "movl", "ml", X_RM, 0x8b },
    { "movq", "Gq", X_RM, REXW | 0x8b },
    { "movq", "Iq", X_MOV_I, REXW },
    { "movq", "iq", X_RM, REXW | 0x8b },
    { "movq", "mq", X_RM, REXW | 0x8b },
    { "movq", "mx", X_RM, PF3 | 0x0f7e },
    { "movq", "qm", X_MR, REXW | 0x89 },
    { "movq", "qq", X_MR, REXW | 0x89 },
    { "movq", "qx", X_RM, P66 | REXW | 0x0f6e },
    { "movq", "xm", X_MR, P66 | 0x0fd6 },
    { "movq", "xq", X_MR, P66 | REXW | 0x0f7e },
    { "movsbl", "bl", X_RM, 0x0fbe },
    { "movsbl", "ml", X_RM, 0x0fbe },
    { "movsbq", "mq", X_RM, REXW | 0x0fbe },
    { "movsd", "mx", X_RM, PF2 | 0x0f10 },
    { "movsd", "xm", X_MR, PF2 | 0x0f11 },
    { "movslq", "iq", X_RM, REXW | 0x63 },
    { "movslq", "lq", X_RM, REXW | 0x63 },
    { "movslq", "mq", X_RM, REXW | 0x63 },
    { "movss", "mx", X_RM, PF3 | 0x0f10 },
    { "movss", "xm", X_MR, PF3 | 0x0f11 },
    { "movswl", "ml", X_RM, 0x0fbf },
    { "movswl", "wl", X_RM, 0x0fbf },
    { "movswq", "mq", X_RM, REXW | 0x0fbf },
    { "movw", "wm", X_MR, P66 | 0x89 },
    { "movzbl", "bl", X_RM, 0x0fb6 },
    { "movzbl", "ml", X_RM, 0x0fb6 },
    { "movzbq", "mq", X_RM, REXW | 0x0fb6 },
    { "movzwl", "ml", X_RM, 0x0fb7 },
    { "movzwl", "wl", X_RM, 0x0fb7 },
    { "movzwq", "mq", X_RM, REXW | 0x0fb7 },
    { "mulq", "q", X_M, REXW | DIG(4) | 0xf7 },
    { "mulsd", "xx", X_RM, PF2 | 0x0f59 },
    { "mulss", "xx", X_RM, PF3 | 0x0f59 },
    { "negl", "l", X_M, DIG(3) | 0xf7 },
    { "negq", "q", X_M, REXW | DIG(3) | 0xf7 },
    { "notl", "l", X_M, DIG(2) | 0xf7 },
    { "notq", "q", X_M, REXW | DIG(2) | 0xf7 },
    { "orb", "bb", X_MR, 0x08 },
    { "orl", "Il", X_ALU_I, DIG(1) },
    { "orl", "ll", X_MR, 0x09 },
    { "orq", "Iq", X_ALU_I, REXW | DIG(1) },
    { "orq", "qq", X_MR, REXW | 0x09 },
    { "pushq", "q", X_O, 0x50 },
    { "rep movsb", "", X_FIXED, PF3 | 0xa4 },
    { "rep stosb", "", X_FIXED, PF3 | 0xaa },
    { "ret", "", X_FIXED, 0xc3 },
    { "rolw", "Iw", X_SHIFT_I, P66 | DIG(0) },
    { "sarl", "Il", X_SHIFT_I, DIG(7) },
    { "sarl", "bl", X_M, DIG(7) | 0xd3 },
    { "sarq", "Iq", X_SHIFT_I, REXW | DIG(7) },
    { "sarq", "bq", X_M, REXW | DIG(7) | 0xd3 },
    { "sbbq", "qq", X_MR, REXW | 0x19 },
    { "seta", "b", X_M, 0x0f97 },
    { "setae", "b", X_M, 0x0f93 },
    { "setb", "b", X_M, 0x0f92 },
    { "setbe", "b", X_M, 0x0f96 },
    { "sete", "b", X_M, 0x0f94 },
    { "setg", "b", X_M, 0x0f9f },
    { "setge", "b", X_M, 0x0f9d },
    { "setl", "b", X_M, 0x0f9c },
    { "setle", "b", X_M, 0x0f9e },
    { "setne", "b", X_M, 0x0f95 },
    { "setnp", "b", X_M, 0x0f9b },
    { "setp", "b", X_M, 0x0f9a },
    { "shldq", "bqq", X_MR, REXW | 0x0fa5 },
    { "shll", "Il", X_SHIFT_I, DIG(4) },
    { "shll", "bl", X_M, DIG(4) | 0xd3 },
    { "shlq", "Iq", X_SHIFT_I, REXW | DIG(4) },
    { "shlq", "bq", X_M, REXW | DIG(4) | 0xd3 },
    { "shrdq", "bqq", X_MR, REXW | 0x0fad },
    { "shrl", "Il", X_SHIFT_I, DIG(5) },
    { "shrl", "bl", X_M, DIG(5) | 0xd3 },
    { "shrq", "Iq", X_SHIFT_I, REXW | DIG(5) },
    { "shrq", "bq", X_M, REXW | DIG(5) | 0xd3 },
    { "sqrtsd", "xx", X_RM, PF2 | 0x0f51 },
    { "sqrtss", "xx", X_RM, PF3 | 0x0f51 },
    { "subl", "Il", X_ALU_I, DIG(5) },
    { "subl", "ll", X_MR, 0x29 },
    { "subq", "Iq", X_ALU_I, REXW | DIG(5) },
    { "subq", "qq", X_MR, REXW | 0x29 },
    { "subsd", "xx", X_RM, PF2 | 0x0f5c },
    { "subss", "xx", X_RM, PF3 | 0x0f5c },
    { "testb", "Ib", X_TEST_IB, 0 },
    { "testb", "bb", X_MR, 0x84 },
    { "testl", "ll", X_MR, 0x85 },
    { "testq", "qq", X_MR, REXW | 0x85 },
    { "ucomisd", "xx", X_RM, P66 | 0x0f2e },
    { "ucomiss", "xx", X_RM, 0x0f2e },
    { "ud2", "", X_FIXED, 0x0f0b },
    { "xorl", "Il", X_ALU_I, DIG(6) },
    { "xorl", "ll", X_MR, 0x31 },
    { "xorq", "Iq", X_ALU_I, REXW | DIG(6) },
    { "xorq", "qq", X_MR, REXW | 0x31 },
};

const AsmArch as_x86_64 = { "x86_64", x86_64_rows, sizeof(x86_64_rows) / sizeof(x86_64_rows[0]), 1, SECSYM_SECTION, x86_64_parse, x86_64_shape, x86_64_insn_size, x86_64_encode, x86_64_pad, elf_write };
