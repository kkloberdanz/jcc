#ifndef IR_H
#define IR_H

#include <stddef.h>
#include <stdio.h>

#include "arch.h"

/*
 * The intermediate representation: linear, non-SSA, three-address code
 * on typed virtual registers, one IRFunc per function. src/lower.c
 * builds it from the typed tree, src/regalloc.c gives every virtual
 * register a place (a machine register or a frame slot), and each
 * backend's `select` turns it into assembly.
 *
 * Everything is referenced by index, never by pointer: blocks,
 * instructions and virtual registers live in arrays on the function, so
 * two builds of the compiler produce the same text (the self-host check
 * compares them byte for byte) and the dump is trivial.
 *
 * Only i32, i64, f32 and f64 are register types in the usual sense; C's
 * promotions never compute in char or short, so narrow widths appear
 * only on load, store, sext and zext. i128 (__int128) and f80 (the x87
 * long double where the target has it) are values too, but ones the
 * backends keep in memory and operate on through scratch registers.
 * Pointers are i64. Records, arrays and anything address-taken never
 * enter a register: they are frame slots or globals, reached by address.
 *
 * A virtual register holding a C value of type T holds it extended to
 * T's IR type: a `char` is an i32 whose upper 24 bits are its sign (or
 * zero for unsigned char), so a load of a narrow object extends and a
 * conversion to a narrow type is a sext or zext of the low bits. Nothing
 * is assumed about the bits above an i32's 32 in the machine register.
 */

typedef enum IRType {
    IR_VOID,
    IR_I32,
    IR_I64,
    IR_I128,
    IR_F32,
    IR_F64,
    IR_F80
} IRType;

/* the X-macro keeps the enum and the dump's spelling together */
#define IR_OPS(X) \
    X(IR_NOP, "nop") \
    /* constants and addresses */ \
    X(IR_ICONST, "iconst")          /* dst = imm (sign extended to the type) */ \
    X(IR_FCONST, "fconst")          /* dst = the bit pattern imm (f32: low 32; f80: mantissa, aux = sign and exponent) */ \
    X(IR_ADDR_LOCAL, "addr_local")  /* dst:i64 = fp + imm */ \
    X(IR_ADDR_GLOBAL, "addr_global")/* dst:i64 = &sym; IRF_EXTERN: defined elsewhere, so through the GOT */ \
    X(IR_ADDR_STRING, "addr_string")/* dst:i64 = &string literal imm */ \
    X(IR_LABEL_ADDR, "label_addr")  /* dst:i64 = the address of the block whose addr_label is imm (GNU &&label) */ \
    X(IR_MOV, "mov")                /* dst = a */ \
    /* memory */ \
    X(IR_LOAD, "load")              /* dst = the `size` bytes at *a, extended to the type (IRF_SIGNED) */ \
    X(IR_STORE, "store")            /* the `size` low bytes of b -> *a */ \
    X(IR_LOADBITS, "loadbits")      /* dst = the bit-field in the `size` byte unit at *a: imm = bit_offset | bits << 8 */ \
    X(IR_STOREBITS, "storebits")    /* the bit-field at *a = b; dst (optional) = the field as stored, extended */ \
    X(IR_COPY, "copy")              /* imm bytes from *b to *a */ \
    X(IR_ZERO, "zero")              /* imm zero bytes at *a */ \
    /* integer arithmetic: dst = a op b, or a op imm with IRF_IMM */ \
    X(IR_ADD, "add") \
    X(IR_SUB, "sub") \
    X(IR_MUL, "mul") \
    X(IR_SDIV, "sdiv") \
    X(IR_UDIV, "udiv") \
    X(IR_SREM, "srem") \
    X(IR_UREM, "urem") \
    X(IR_SHL, "shl") \
    X(IR_LSHR, "lshr") \
    X(IR_ASHR, "ashr") \
    X(IR_AND, "and") \
    X(IR_OR, "or") \
    X(IR_XOR, "xor") \
    X(IR_NEG, "neg") \
    X(IR_NOT, "not") \
    /* floating point */ \
    X(IR_FADD, "fadd") \
    X(IR_FSUB, "fsub") \
    X(IR_FMUL, "fmul") \
    X(IR_FDIV, "fdiv") \
    X(IR_FNEG, "fneg") \
    X(IR_FSQRT, "fsqrt")            /* dst = sqrt(a): -O1 makes it of sqrt, which sets no errno on the targets' libcs */ \
    X(IR_FABS, "fabs") \
    X(IR_FRINT, "frint")            /* dst = a rounded to an integer: imm 0 floor, 1 ceil, 2 trunc, 3 round (ArchInfo.fp_round targets) */ \
    /* comparisons: dst:i32 = 0 or 1; `type` is the operands' */ \
    X(IR_ICMP, "icmp") \
    X(IR_FCMP, "fcmp") \
    /* conversions */ \
    X(IR_SEXT, "sext")              /* dst = the low `size` bytes of a, sign extended to the type */ \
    X(IR_ZEXT, "zext")              /* the same, zero extended */ \
    X(IR_TRUNC, "trunc")            /* dst = the low bits of a (a is wider) */ \
    X(IR_ITOF, "itof")              /* dst = (float) a, a of integer type t2 (IRF_SIGNED) */ \
    X(IR_FTOI, "ftoi")              /* dst = (integer) a, truncated, a of floating type t2 (IRF_SIGNED) */ \
    X(IR_FTOF, "ftof")              /* dst = a converted between floating types (from t2) */ \
    X(IR_BITCAST, "bitcast")        /* dst = the bits of a (i32 <-> f32, i64 <-> f64) */ \
    /* the builtins that are instructions */ \
    X(IR_BSWAP, "bswap")            /* dst = the low `size` bytes of a reversed */ \
    X(IR_CLZ, "clz")                /* leading zeros of a (type i32 or i64) */ \
    X(IR_CTZ, "ctz") \
    X(IR_FENCE, "fence") \
    X(IR_ALLOCA, "alloca")          /* dst:i64 = sp after taking a bytes (rounded up to 16) */ \
    X(IR_SPSAVE, "spsave")          /* dst:i64 = sp (a variable length array's allocation point) */ \
    X(IR_SPRESTORE, "sprestore")    /* sp = a, a value spsave gave: frees what was allocated since */ \
    X(IR_TLS_ADDR, "tlsaddr")       /* dst:i64 = this thread's address of the thread-local `sym` (IRF_EXTERN as for addr_global); the backends call the runtime, so it clobbers registers like a call */ \
    /* control; every block ends in one of these five */ \
    X(IR_BR, "br")                  /* to block imm */ \
    X(IR_CONDBR, "condbr")          /* a != 0: block imm, else block aux */ \
    X(IR_RET, "ret")                /* a (or none); IRF_RECORD: a is the address of a record returned in registers, imm its ArgType index */ \
    X(IR_UNREACHABLE, "unreachable") \
    X(IR_SWITCH, "switch")          /* a (i64) < n: to block switches[imm] entry a, else to block aux; only from -O1 */ \
    X(IR_IJMP, "ijmp")              /* GNU `goto *a`: to the address in a, the start of one of the blocks switches[imm] lists */ \
    X(IR_NLGOTO, "nlgoto")          /* a jump out of a GNU nested function to its label in the enclosing function whose frame */ \
                                    /*   pointer is a: sp = *(a + imm), fp = a, then to the address in b; with imm -1 */ \
                                    /*   (__builtin_longjmp) a is the buffer: fp = a[0], to a[1], sp = a[2] */ \
    X(IR_ASMBR, "asmbr")            /* after an asm goto: on to block switches[imm] entry 0, unless the asm jumped to a later one */ \
    /* calls and variadic functions */ \
    X(IR_CALL, "call")              /* imm = index into calls[]; dst = the scalar result, or none */ \
    X(IR_VASTART, "va_start")       /* a = the address of the va_list */ \
    X(IR_VAARG, "va_arg")           /* dst:i64 = the address of the next argument of the va_list at *a; aux = ArgType index; imm = a 16 byte frame slot, 0 if none */

typedef enum IROp {
#define X(name, repr) name,
    IR_OPS(X)
#undef X
    IR_NOPS
} IROp;

/* comparison conditions; the unsigned ones for integers only */
typedef enum IRCond {
    IR_EQ, IR_NE,
    IR_LT, IR_GE, IR_GT, IR_LE,         /* signed, or ordered for floats */
    IR_ULT, IR_UGE, IR_UGT, IR_ULE
} IRCond;

/* IRInst.flags */
#define IRF_SIGNED  1u      /* load, loadbits, sext/zext (implied), itof, ftoi: the integer is signed */
#define IRF_IMM     2u      /* arithmetic and icmp: the second operand is imm, not b */
#define IRF_EXTERN  4u      /* addr_global: not defined in this unit */
#define IRF_RECORD  8u      /* ret: a record returned in registers */
#define IRF_VOLATILE 16u    /* load, store, loadbits, storebits: a volatile object, which the optimizer leaves alone */
#define IRF_NSW     32u     /* i32 add, sub, mul: C's signed int arithmetic, whose overflow is undefined (-O1 relies on it) */
#define IRF_APPLY   64u     /* ret: __builtin_return, a is __builtin_apply's results block (rax, rdx, xmm0, xmm1) */

typedef struct IRInst {
    unsigned char op;       /* IROp */
    unsigned char type;     /* IRType of dst, or of the operands for store, icmp, fcmp */
    unsigned char t2;       /* IRType of the source for itof, ftoi, ftof, bitcast */
    unsigned char size;     /* bytes: load, store, bits, sext, zext, bswap */
    unsigned char flags;    /* IRF_* */
    unsigned char cc;       /* IRCond for icmp and fcmp */
    int dst;                /* virtual register, or -1 */
    int a;                  /* operand virtual registers, -1 when unused */
    int b;
    int aux;                /* condbr: the else block; fconst f80: sign and exponent; va_arg: ArgType index */
    long imm;               /* an immediate, frame offset, block, string label or side table index */
    const char *sym;        /* addr_global: the symbol's assembler name */
} IRInst;

/* a block is a contiguous run of instructions ending in a terminator */
typedef struct IRBlock {
    int start;
    int end;                /* one past the last instruction */
    int addr_label;         /* GNU &&label takes its address: the unit-wide number of its assembler label, else 0 */
} IRBlock;

typedef struct IRVReg {
    unsigned char type;     /* IRType */
    unsigned char is_local; /* a C variable: it may be read before it is written (the verifier does not check it) */
    const char *name;       /* the C variable's name, for the dump; NULL for a temporary */
    int nuses;              /* reads, counted by ir_count_uses */
    int def;                /* the (last) instruction that writes it, -1 if none */
    int use;                /* the (last) instruction that reads it, -1 if none */
    int folded;             /* set by the backend before allocation: an address it folds into the memory access that uses it, so it needs no place */
    /* set by the allocator */
    int reg;                /* an index into the target's register class for its type (integer or floating), or -1 */
    long offset;            /* its frame slot when reg is -1; 0 when it needs none (never read, or folded) */
} IRVReg;

/* one argument of a call */
typedef struct IRArg {
    int v;                  /* the value, or the address of a record or complex passed by value */
    int by_addr;
    ArgType type;
} IRArg;

typedef struct IRCall {
    const char *name;       /* the callee's C name for a direct call, NULL for an indirect one */
    int target;             /* indirect: the virtual register holding the function's address */
    int first_arg;          /* into IRFunc.args */
    int nargs;
    size_t nfixed;          /* named parameters; the rest are variadic */
    int variadic;           /* the callee's type is variadic */
    int sret_addr;          /* the virtual register with the address a record result is written through, or -1 */
    int result_addr;        /* the virtual register with the address of the temporary a record returned in registers is stored to, or -1 */
    ArgType ret;            /* result_addr: how the record comes back */
    int asm_index;          /* an asm statement, not a call: IRFunc.asms[asm_index - 1]; 0 for a call */
    long chain_slot;        /* a call of a GNU nested function: the frame slot holding its static chain, which the
                               backend puts in the chain register (x86_64 r11, arm64 x15) for it; 0 for none */
} IRCall;

/*
 * An asm statement is a call (IRCall.asm_index) for everything but the
 * backend: its arguments are the addresses of the frame slots its
 * operands go through and of its memory operands, so the optimizer sees
 * those objects escape and be changed, and it clobbers what a call does.
 * The backend places the operands, writes the template with them, and
 * stores the outputs.
 */
typedef struct IRAsmOp {
    char kind;              /* 'r' a register, 'm' memory (its address is in the slot), 'i' a constant */
    char specific;          /* x86: the register the constraint letter names, else 0 */
    char fp;                /* a floating point register */
    char is_output;
    char is_input;          /* read before the asm: an input, or an in-out output (+) */
    int match;              /* an input sharing output `match`'s register, else -1 */
    int size;               /* bytes of the value */
    long slot;              /* the frame slot: the value, or for 'm' the address */
    long imm;               /* 'i' */
    const char *name;       /* [name], or NULL */
} IRAsmOp;

typedef struct IRAsm {
    const char *text;       /* the template; for a basic asm, the text to write as it is */
    int basic;
    IRAsmOp *ops;           /* outputs then inputs, as the template numbers them */
    int nops;
    const char **clobbers;  /* register names (and "memory", "cc") */
    int nclobbers;
    int nlabels;            /* asm goto: its labels, numbered after the operands, are the targets of the asmbr after it */
} IRAsm;

/* a jump table: IR_SWITCH's targets for the indices 0..n-1, at IRFunc.jt[first] */
typedef struct IRSwitch {
    int first;
    int n;
} IRSwitch;

/* a parameter: where it arrives (loc) and where it lives */
typedef struct IRParam {
    ArgLoc loc;
    int vreg;               /* its virtual register, or -1 when it lives in memory */
    long offset;            /* vreg == -1: its frame slot (a stack parameter's is positive, in the caller's area) */
    size_t size;            /* bytes of its type */
    int is_signed;          /* an integer narrower than 4 bytes: extended on entry, in case the caller did not */
    int is_float;
    int wide;               /* an __int128: two registers */
    int by_addr;            /* a record (loc.by_ref: the slot holds its address) */
    long kr_from;           /* a K&R float that arrived as a double: the slot the double is stored to first, 0 if not one */
} IRParam;

typedef struct IRFunc {
    const char *name;
    int is_global;          /* 0 static, 1 global, 2 global and weak */
    int is_variadic;
    int is_main;
    int opt_level;          /* -O: at 1 the backends and the allocator may also do what -O0 does not */
    int noopt;              /* calls setjmp: the optimizer leaves it as it is, and does not inline it */
    IRBlock *blocks;
    int nblocks;
    int blocks_cap;
    IRInst *insts;
    int ninsts;
    int insts_cap;
    IRVReg *vregs;
    int nvregs;
    int vregs_cap;
    IRParam *params;
    int nparams;
    IRCall *calls;
    int ncalls;
    int calls_cap;
    IRArg *args;
    int nargs;
    int args_cap;
    IRSwitch *switches;     /* for switch */
    int nswitches;
    int switches_cap;
    int *jt;                /* the switches' target blocks */
    int njt;
    int jt_cap;
    ArgType *argtypes;      /* for ret and va_arg */
    int nargtypes;
    int argtypes_cap;
    IRAsm *asms;            /* the asm statements (IRCall.asm_index) */
    int nasms;
    int asms_cap;
    char **names;           /* copies of symbol names the instructions point at, freed with the function */
    int nnames;
    int names_cap;
    long locals_size;       /* bytes below the frame pointer that lowering gave to locals and temporaries, a multiple of 16 */
    long sret_off;          /* the slot holding the hidden return pointer, 0 when there is none */
    long apply_args_off;    /* __builtin_apply_args: the block the prologue saves the argument registers in (rdi..r9 at 8,
                               rax at 56, xmm0..7 at 64), the stack arguments' address at 0; else 0 */
    long chain_off;         /* a GNU nested function: the slot the prologue stores its static chain register in, else 0 */
    size_t align;           /* __attribute__((aligned(n))) on the function: where its code starts, else 0 */
    int save_all;           /* a nested function may jump back into this one past the epilogues of the frames between:
                               it saves and restores every callee-saved register, as their callers expect of it */
    size_t nreg_params;     /* argument registers the named parameters used (va_start needs them) */
    size_t nfreg_params;
    size_t nstack_params;
    int cur;                /* the block instructions are appended to, -1 when the last one ended in a terminator */
    /* set by the allocator */
    long frame_size;        /* the whole frame, a multiple of 16 */
    int saved_ints[32];     /* the callee-saved registers in use (class indices, as IRVReg.reg), ints then floats, */
    int nsaved_ints;        /*   saved_off + 8k is the frame offset of the k-th one */
    int saved_floats[32];
    int nsaved_floats;
    long saved_off;
} IRFunc;

/* building */

IRFunc *ir_func_new(const char *name);
void ir_func_free(IRFunc *f);
int ir_vreg(IRFunc *f, IRType type);                        /* a new temporary */
int ir_vreg_local(IRFunc *f, IRType type, const char *name);/* a new register for a C variable */
int ir_block(IRFunc *f);                                    /* a new, empty block (not yet the current one) */
void ir_place(IRFunc *f, int block);                        /* make `block` current: the one before falls through to it */
/* append an instruction; returns its index. The current block is opened if the last ended */
int ir_emit(IRFunc *f, IROp op, IRType type, int dst, int a, int b);
IRInst *ir_inst(IRFunc *f, int index);
int ir_last(IRFunc *f);                                     /* the index of the last instruction emitted */
int ir_add_call(IRFunc *f, const IRCall *c);                /* copies; returns its index */
int ir_add_asm(IRFunc *f, const IRAsm *a);                  /* takes its ops and clobbers arrays; returns its index */
int ir_add_arg(IRFunc *f, const IRArg *a);
int ir_add_argtype(IRFunc *f, const ArgType *t);
int ir_add_switch(IRFunc *f, const int *targets, int n);   /* a jump table; returns its index */
int ir_param_add(IRFunc *f, const IRParam *p);
const char *ir_name(IRFunc *f, const char *name);           /* a copy of `name` that lives as long as the function */

/* the convenient forms lowering uses; each returns the destination */
int ir_iconst(IRFunc *f, IRType type, long value);
int ir_binop(IRFunc *f, IROp op, IRType type, int a, int b);
int ir_binop_imm(IRFunc *f, IROp op, IRType type, int a, long imm);
int ir_unop(IRFunc *f, IROp op, IRType type, int a);
int ir_icmp(IRFunc *f, IRCond cc, IRType type, int a, int b);
int ir_icmp_imm(IRFunc *f, IRCond cc, IRType type, int a, long imm);
int ir_load(IRFunc *f, IRType type, int addr, size_t size, int is_signed);
void ir_store(IRFunc *f, IRType type, int addr, int value, size_t size);
void ir_br(IRFunc *f, int block);
void ir_condbr(IRFunc *f, int cond, int then_block, int else_block);
void ir_mov(IRFunc *f, int dst, int src);

/* queries */

const char *ir_op_name(IROp op);
const char *ir_type_name(IRType t);
size_t ir_type_size(IRType t);
int ir_type_is_float(IRType t);
int ir_is_terminator(IROp op);
int ir_nsuccs(const IRFunc *f, const IRInst *t);            /* the successors of terminator t, with repeats: */
int ir_succ(const IRFunc *f, const IRInst *t, int k);       /*   br: imm; condbr: imm, aux; switch: aux, then the table */
void ir_set_succ(IRFunc *f, IRInst *t, int k, int block);
void ir_count_uses(IRFunc *f);          /* fill in IRVReg.nuses, def and use */
int ir_clobbers_registers(const IRFunc *f, const IRInst *i);   /* a call, or an operation the backends call the runtime for */

/* the text form; the verifier returns the number of problems, each reported as an internal error */
void ir_dump(const IRFunc *f, FILE *out);
int ir_verify(IRFunc *f);

#endif /* IR_H */
