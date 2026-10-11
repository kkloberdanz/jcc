#ifndef OPT_H
#define OPT_H

#include "ir.h"

/*
 * The optimizer behind -O1. It rewrites one function's IR in place,
 * between lowering and instruction selection, and leaves IR of the same
 * form behind: the backends and the allocator see nothing they would
 * not see at -O0, only less of it.
 *
 * Inside, the function is put into SSA form (dominators, phis at the
 * iterated dominance frontiers, renaming), simplified there (constant
 * folding, algebraic identities, copy propagation, global value
 * numbering with redundant load elimination, branch folding, dead code
 * elimination), and taken out of SSA again by coalescing each phi with
 * its arguments where their live ranges allow and inserting parallel
 * copies on the (split) edges where they do not. The CFG is then
 * cleaned up (empty blocks threaded, straight-line blocks merged) and
 * the virtual registers renumbered densely.
 *
 * Everything depends only on instruction, block and register numbers,
 * so the output is deterministic (the self-host check compares stages
 * byte for byte).
 */
void opt_function(IRFunc *f);

/*
 * A translation unit's functions, in order: each is optimized after the
 * functions it calls (callees first, so that a small or single-use one
 * is already in its final form when it is inlined into its callers).
 * droppable[k] says that function k is static and its address is never
 * taken, so that it need not be emitted once no call to it remains;
 * emit[k] is set for the functions to emit. A function that fails the
 * verifier afterwards has been reported as an internal error.
 */
void opt_unit(IRFunc **funcs, int n, const char *droppable, char *emit);

#endif /* OPT_H */
