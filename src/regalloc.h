#ifndef REGALLOC_H
#define REGALLOC_H

#include "ir.h"

/*
 * Give every virtual register of a function a place: a machine register
 * or a frame slot. Liveness is a backward dataflow over the CFG, then
 * one live interval per virtual register (it is not SSA: a C local is
 * one register assigned as often as the program assigns it, and its
 * interval runs from its first live point to its last) is handed to a
 * linear scan over the target's register file. A value live across a
 * call sits in a callee-saved register or in a slot; the backends load
 * a slot's value into scratch registers around each instruction as in
 * Step 17, so a spilled register costs a load or a store per use and
 * nothing is inserted into the IR.
 *
 * A class is the target's registers of one kind in three tiers:
 *   [0, nargs)        the argument and result registers, written by the
 *                     call sequence before the call and by whatever
 *                     instructions the target's `clobbers` says, so a
 *                     value may hold one only if no such instruction
 *                     lies between its definition and its last use (a
 *                     call's operands included: they are read while the
 *                     argument registers are being loaded);
 *   [nargs, ncaller)  the other caller-saved registers, for values not
 *                     live across a call;
 *   [ncaller, n)      the callee-saved registers, saved in the prologue
 *                     and restored in the epilogue when used.
 * The target's scratch registers, the ones its selectors use for slot
 * values and intermediate results, are in no class.
 *
 * Sets IRVReg.reg (an index into its class's `regs`, or -1), IRVReg.offset
 * (a slot, or 0 for a value that needs none), IRFunc.frame_size and the
 * saved register lists (IRFunc.saved_*). Everything depends only on
 * instruction and register numbers, so two builds of the compiler agree
 * byte for byte.
 */

typedef struct RegClass {
    const int *regs;        /* machine register numbers */
    int nargs;
    int ncaller;
    int n;
    int ret;                /* the index of the register a result is returned in, -1 if it is not in the class */
} RegClass;

/* what an instruction's own code writes besides its destination, from RegTarget.clobbers */
#define RA_CLOBBER_NONE 0
#define RA_CLOBBER_ARGS 1   /* the argument tier */
#define RA_CLOBBER_CALL 2   /* every caller-saved register (a call, or a runtime call the selector makes) */

typedef struct RegTarget {
    RegClass ints;          /* i32 and i64 */
    RegClass floats;        /* f32 and f64 */
    int (*clobbers)(const IRFunc *f, const IRInst *i);
} RegTarget;

void regalloc(IRFunc *f, const RegTarget *t);

/* an __int128 or x87 long double operand: the selectors compute those through scratch register pairs */
int regalloc_has_wide(const IRFunc *f, const IRInst *i);

#endif /* REGALLOC_H */
