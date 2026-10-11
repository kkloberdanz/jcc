#ifndef ASM_H
#define ASM_H

#include <stddef.h>

#include "as.h"

/*
 * The assembler's internals, shared by the line reader (asm.c), the two
 * targets' operand parsers and instruction tables (arm64.c, x86_64.c)
 * and the object writers. The whole unit is parsed into sections of
 * items (bytes, labels, alignment, instructions with their operands
 * decoded) before anything is encoded, so that a target can size
 * instructions whose length depends on label distances.
 */

/* ------------------------------------------------------------------ */
/* tokens: what a line is cut into before the target sees it           */
/* ------------------------------------------------------------------ */

typedef enum AsmTokKind {
    ATOK_END,
    ATOK_IDENT,      /* [A-Za-z_.][A-Za-z0-9_.]*, so `.L3`, `L.str.0`, `.` and `_x.1` are one token */
    ATOK_NUM,        /* decimal or 0x hexadecimal; the sign is a separate '-' */
    ATOK_FNUM,       /* a decimal fraction, 2.5 (no exponent): num holds the double's bits */
    ATOK_STRING,     /* "..." with \", \\ and \ooo escapes decoded */
    ATOK_PUNCT       /* one of , [ ] ! # $ % ( ) * + - : @ */
} AsmTokKind;

typedef struct AsmTok {
    AsmTokKind kind;
    const char *s;          /* ATOK_IDENT, ATOK_STRING: the text (a string's bytes, decoded) */
    size_t len;
    unsigned long num;      /* ATOK_NUM: the value, wrapped modulo 2^64 */
    char punct;             /* ATOK_PUNCT */
} AsmTok;

#define MAX_TOKS 32

/* ------------------------------------------------------------------ */
/* operands                                                            */
/* ------------------------------------------------------------------ */

typedef enum AsmOpKind {
    OP_NONE,
    OP_REG,         /* reg, cls */
    OP_IMM,         /* imm: #n on arm64, $n on x86_64 */
    OP_FIMM,        /* arm64 #2.5: imm holds the double's bits */
    OP_SYM,         /* sym with `mode` saying how it is referenced (SYM_PLAIN, SYM_PAGE, ...) and `imm` an addend */
    OP_MEM,         /* a memory operand: base, index, scale, imm (the displacement), sym, mode (MEM_...) */
    OP_SHIFT,       /* arm64 `lsl #n`: reg is the shift kind (SHIFT_LSL ...), imm the amount */
    OP_COND,        /* arm64 condition code: imm is its number */
    OP_BARRIER      /* arm64 `ish` */
} AsmOpKind;

/* OP_SYM modes, and the symbol part of a memory operand */
enum {
    SYM_PLAIN,      /* name, a label or symbol */
    SYM_PAGE,       /* name@PAGE */
    SYM_PAGEOFF,    /* name@PAGEOFF */
    SYM_GOTPAGE,    /* name@GOTPAGE */
    SYM_GOTPAGEOFF, /* name@GOTPAGEOFF */
    SYM_PLT,        /* name@PLT */
    SYM_GOTPCREL    /* name@GOTPCREL */
};

/* OP_MEM modes */
enum {
    MEM_BASE,       /* [Xn] [Xn, #imm]; (%r) disp(%r) */
    MEM_PRE,        /* arm64 [Xn, #imm]! */
    MEM_POST,       /* arm64 [Xn], #imm */
    MEM_INDEX,      /* arm64 [Xn, Xm]; x86_64 disp(%base, %index, scale) */
    MEM_SYM,        /* arm64 [Xn, sym@GOTPAGEOFF]; x86_64 sym(%rip) and sym@GOTPCREL(%rip) with `base` the rip marker */
    MEM_INDIRECT    /* x86_64 *%reg: a register operand marked indirect; reg and cls are set */
};

enum { SHIFT_LSL, SHIFT_LSR, SHIFT_ASR };

#define NO_REG 0xff

typedef struct AsmOp {
    unsigned char kind;     /* AsmOpKind */
    unsigned char cls;      /* OP_REG, MEM_INDIRECT: the register class, the target's numbering */
    unsigned char reg;      /* OP_REG: the number; OP_SHIFT: the kind */
    unsigned char mode;     /* OP_SYM: SYM_...; OP_MEM: MEM_... */
    unsigned char base;     /* OP_MEM: registers, NO_REG for none */
    unsigned char index;
    unsigned char scale;
    long imm;               /* OP_IMM; OP_MEM and OP_SYM: displacement or addend; OP_SHIFT: amount; OP_COND: the code */
    int sym;                /* OP_SYM, OP_MEM: the symbol's index in the unit's table, or -1 */
} AsmOp;

#define MAX_OPS 4

typedef struct AsmInst {
    short row;              /* the entry of the target's table this instruction matched */
    unsigned char nops;
    AsmOp ops[MAX_OPS];
} AsmInst;

/* ------------------------------------------------------------------ */
/* sections, symbols, items                                            */
/* ------------------------------------------------------------------ */

typedef enum SecKind { SEC_TEXT, SEC_DATA, SEC_BSS, SEC_RODATA, SEC_NOTE, SEC_COUNT } SecKind;

typedef enum ItemKind {
    ITEM_BYTES,     /* u.bytes: a run of the section's data buffer */
    ITEM_INSN,      /* u.insn */
    ITEM_LABEL,     /* u.sym: the symbol is defined here */
    ITEM_ALIGN,     /* u.p2: .p2align */
    ITEM_ADDR,      /* u.addr: .quad sym+addend, 8 bytes to be relocated */
    ITEM_SIZE,      /* u.sym: .size sym, .-sym at this point */
    ITEM_DIFF       /* u.diff: .long a-b, two labels of this section: 4 bytes, the distance */
} ItemKind;

typedef struct AsmItem {
    unsigned char kind;     /* ItemKind */
    unsigned line;
    size_t off;             /* where it starts in its section, once laid out */
    size_t size;            /* ITEM_INSN: its encoded length, once laid out */
    union {
        struct { size_t off; size_t len; } bytes;
        AsmInst insn;
        int sym;
        int p2;
        struct { int sym; long addend; } addr;
        struct { int a; int b; } diff;
    } u;
} AsmItem;

/* how a relocated field refers to its symbol; the object writer maps these to the format's types */
enum {
    R_ABS64,                /* an 8-byte address (.quad sym+addend) */
    R_BRANCH26,             /* arm64 b, bl */
    R_PAGE21,               /* arm64 adrp sym@PAGE */
    R_PAGEOFF12,            /* arm64 add/ldr sym@PAGEOFF */
    R_GOT_PAGE21,           /* arm64 adrp sym@GOTPAGE */
    R_GOT_PAGEOFF12,        /* arm64 ldr [Xn, sym@GOTPAGEOFF] */
    R_PC32,                 /* x86_64 sym(%rip) */
    R_PLT32,                /* x86_64 call sym@PLT */
    R_GOTPCRELX,            /* x86_64 sym@GOTPCREL(%rip) in an instruction without a REX prefix */
    R_REX_GOTPCRELX         /* the same with one (movq sym@GOTPCREL(%rip), %r64) */
};

typedef struct AsmReloc {
    size_t off;             /* of the field in the section */
    int sym;
    long addend;
    unsigned char kind;     /* R_... */
} AsmReloc;

typedef struct AsmSection {
    AsmItem *items;
    size_t n;
    size_t cap;
    unsigned char *data;    /* ITEM_BYTES contents, in order */
    size_t data_len;
    size_t data_cap;
    int used;               /* the unit entered it */
    int order;              /* its place among the used sections, in order of first entry (text is 0) */
    /* after layout and encoding */
    size_t size;
    int p2align;            /* the largest .p2align seen in it */
    unsigned char *out;     /* the encoded bytes (NULL for .bss) */
    AsmReloc *relocs;
    size_t nrelocs;
    size_t relocs_cap;
} AsmSection;

typedef struct AsmSym {
    char *name;
    int section;            /* SecKind, or -1 while undefined */
    size_t item;            /* the ITEM_LABEL that defines it */
    int global;
    int local;              /* an assembler-local label (L..., .L...): not in the object's symbol table */
    int is_func;            /* .type @function */
    int sized;              /* has a .size */
    int is_obj;             /* .type @object */
    int is_section;         /* ELF: the section symbol made when its section was first entered */
    size_t off;             /* in its section, once laid out */
    size_t size;            /* .size sym, .-sym: the distance, once laid out */
    int atom;               /* Mach-O: the nearest non-local symbol defined at or before it in its section */
} AsmSym;

/* the symbol made for a section when the unit first enters it */
enum {
    SECSYM_NONE,
    SECSYM_LTMP,            /* Mach-O: a local `ltmpN` label, the atom that local labels are relocated against */
    SECSYM_SECTION          /* ELF: the nameless STT_SECTION symbol that local symbols are relocated against */
};

/* ------------------------------------------------------------------ */
/* the unit being assembled                                            */
/* ------------------------------------------------------------------ */

typedef struct As {
    const struct AsmArch *arch;
    const char *name;       /* for diagnostics */
    unsigned line;          /* the current line, from 1 */
    int errors;
    AsmSection sec[SEC_COUNT];
    int cur;                /* the current section */
    int nused;              /* how many sections have been entered */
    AsmSym *syms;
    size_t nsyms;
    size_t syms_cap;
    int *buckets;           /* the symbol hash table: indices into syms, -1 for empty */
    size_t nbuckets;
} As;

/*
 * A target: its register names, how an operand is spelled, and its table
 * of (mnemonic, operand shape) rows. `parse` reads the operands of one
 * instruction from the tokens after the mnemonic into `out`, reporting
 * and returning -1 for a spelling it does not know; `shape` writes the
 * letters describing `out`'s operands (one per operand, the target's own
 * alphabet) so that the row can be looked up. The table is sorted by
 * mnemonic then shape; `nrows` is its length.
 */
typedef struct AsmRow {
    const char *mnem;
    const char *shape;
    unsigned char fam;      /* the target's encoding family for the row */
    unsigned long op;       /* the opcode bits the family starts from */
} AsmRow;

typedef struct AsmArch {
    const char *name;
    const AsmRow *rows;
    size_t nrows;
    int local_prefix_dot;   /* local labels are `.L...` (ELF) rather than `L...` (Mach-O) */
    int secsym;             /* SECSYM_...: the symbol a section gets when it is first entered */
    int (*parse)(As *a, char *mnem, const AsmTok *t, size_t n, AsmInst *out);
    void (*shape)(const AsmInst *in, char *buf);
    /* the encoded length of an instruction, given the current layout (symbol offsets) */
    size_t (*insn_size)(As *a, int sec, const AsmItem *it);
    /* encode an instruction at it->off into out (insn_size bytes), recording relocations; 0 or -1 after an error */
    int (*encode)(As *a, int sec, const AsmItem *it, unsigned char *out);
    /* fill `n` bytes of alignment padding in .text */
    void (*pad)(unsigned char *out, size_t n);
    /* write the object: the sections' bytes and relocations, the symbols */
    int (*write)(As *a, char **out, size_t *out_len);
} AsmArch;

/* for the targets: report an error on the current line (the count goes up; parsing continues) */
void as_error(As *a, const char *fmt, ...);
/* the symbol named by `len` bytes of `s`, created undefined if new */
int as_symbol(As *a, const char *s, size_t len);
/* a token is this identifier */
int as_tok_is(const AsmTok *t, const char *s);
/* for the encoders: a relocation of the field at `off` in section `sec` */
void as_reloc(As *a, int sec, size_t off, int sym, long addend, int kind);
/* the object writers */
int macho_write(As *a, char **out, size_t *out_len);
int elf_write(As *a, char **out, size_t *out_len);

#endif /* ASM_H */
