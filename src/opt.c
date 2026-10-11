/*
 * The -O1 optimizer (src/opt.h). One function at a time:
 *
 *   load        the IR's blocks become lists of instruction indices
 *               into a private pool, so that instructions can be added,
 *               removed and moved; unreachable blocks are dropped
 *   ssa         dominators (Cooper, Harvey and Kennedy), dominance
 *               frontiers, phis for every register that is assigned in
 *               more than one place and read across blocks, renaming
 *               along the dominator tree
 *   simplify    in reverse postorder: constant folding, algebraic
 *               identities, copy propagation, value numbering over the
 *               dominator tree, redundant load and store-to-load
 *               elimination within extended blocks, branches on
 *               constants folded; then dead code, repeated while
 *               anything changes
 *   sink        a compare used only by its block's branch, and an
 *               address used only by one access, move next to their
 *               user so the backends fuse them as at -O0
 *   leave ssa   critical edges into blocks with phis are split; each
 *               phi is coalesced with the arguments whose live ranges
 *               do not overlap its own, and parallel copies at the end
 *               of the predecessors move the rest
 *   cleanup     blocks that only jump are threaded, a block whose only
 *               predecessor jumps to it is merged into it, and the
 *               registers are renumbered densely
 *
 * Registers of the four register types are optimized; __int128 and x87
 * long double values (which the backends keep in memory) are left as
 * they are, along with every instruction that touches one.
 *
 * KCC_NOOPT=1 in the environment turns the optimizer off, and
 * KCC_OPT_LIMIT=n optimizes only the first n functions compiled, for
 * bisecting a miscompilation to one function, and KCC_INLINE_LIMIT=n
 * inlines only the first n calls, to bisect to one inlining;
 * KCC_OPT_TRACE=1 reports each function optimized and each call inlined
 * (as notes), to name the nth.
 */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "ir.h"
#include "opt.h"
#include "util.h"

/* a phi: dst = the argument (OptPhi.v[k]) for the predecessor control came from (OptBlock.preds[k]); aux is the register it was made for */
#define OPT_PHI IR_NOPS

/* iconst or fconst 0 with this aux: the value of a register read before it is written, which may be anything */
#define OPT_UNDEF (-2)

typedef unsigned long OptWord;
#define OPT_WBITS 64

typedef struct OptBlock {
    int *code;          /* instructions, indices into opt_I; phis first */
    int n;
    int cap;
    int *preds;
    int npreds;
    int pcap;
    int live;           /* reachable from the entry */
    int rpo;            /* its place in reverse postorder */
    int idom;
    int child;          /* the dominator tree: first child, next sibling */
    int sibling;
    int pre;            /* the dominator tree's preorder interval: a dominates b when a's contains b's */
    int post;
    int after;          /* made by the optimizer: emitted after this block; -1 for the function's own */
    int inlined;        /* a copy of an inlined function's block, whose calls are not inlined again */
    int order;          /* the function's own: where lowering emitted its code, which is the source order */
    int *df;            /* dominance frontier */
    int ndf;
    int dfcap;
} OptBlock;

typedef struct OptPhi {
    int *v;
    int n;
} OptPhi;

/* the function being optimized and its working form */
static IRFunc *opt_F = NULL;
static IRInst *opt_I = NULL;
static int opt_ni = 0;
static int opt_capi = 0;
static OptBlock *opt_B = NULL;
static int opt_nb = 0;
static int opt_capb = 0;
static OptPhi *opt_P = NULL;
static int opt_np = 0;
static int opt_capp = 0;
static int *opt_rpo = NULL;     /* the live blocks in reverse postorder */
static int opt_nrpo = 0;
static int opt_caprpo = 0;

/* per virtual register, opt_vcap of each */
static int opt_vcap = 0;
static int *opt_def = NULL;     /* the instruction that defines it (SSA), -1 for none (a parameter) */
static int *opt_repl = NULL;    /* the register it was found equal to, -1 */
static int *opt_nuse = NULL;
static int *opt_user = NULL;    /* the last instruction reading it */
static int *opt_vstamp = NULL;
static int *opt_vaux = NULL;
static char *opt_vflag = NULL;

/* per instruction in the pool, opt_icap of each */
static int opt_icap = 0;
static int *opt_blk = NULL;     /* its block */
static int *opt_pos = NULL;     /* its index in the block */
static char *opt_mark = NULL;

static int opt_nvars;           /* registers below this were the function's own when it entered SSA */
static int opt_round;           /* the simplification round, from 0 */
static int opt_trace = 0;       /* KCC_OPT_TRACE: each function optimized and each call inlined is reported as a note */
static long opt_inline_limit = -1;  /* KCC_INLINE_LIMIT: inline only this many calls, for bisection */
static long opt_ninlined = 0;

static void *opt_grow(void *p, int *cap, int need, size_t elem) {
    if (need > *cap) {
        int c = *cap ? *cap : 16;
        while (c < need) {
            c *= 2;
        }
        p = xrealloc(p, (size_t)c * elem);
        *cap = c;
    }
    return p;
}

/* room in the per-register arrays for every register the function has */
static void opt_vroom(void) {
    int n = opt_F->nvregs + 1;
    int c;
    if (n <= opt_vcap) {
        return;
    }
    c = opt_vcap ? opt_vcap : 64;
    while (c < n) {
        c *= 2;
    }
    opt_def = xrealloc(opt_def, (size_t)c * sizeof(int));
    opt_repl = xrealloc(opt_repl, (size_t)c * sizeof(int));
    opt_nuse = xrealloc(opt_nuse, (size_t)c * sizeof(int));
    opt_user = xrealloc(opt_user, (size_t)c * sizeof(int));
    opt_vstamp = xrealloc(opt_vstamp, (size_t)c * sizeof(int));
    opt_vaux = xrealloc(opt_vaux, (size_t)c * sizeof(int));
    opt_vflag = xrealloc(opt_vflag, (size_t)c);
    opt_vcap = c;
}

static void opt_iroom(void) {
    int n = opt_ni + 1;
    int c;
    if (n <= opt_icap) {
        return;
    }
    c = opt_icap ? opt_icap : 256;
    while (c < n) {
        c *= 2;
    }
    opt_blk = xrealloc(opt_blk, (size_t)c * sizeof(int));
    opt_pos = xrealloc(opt_pos, (size_t)c * sizeof(int));
    opt_mark = xrealloc(opt_mark, (size_t)c);
    opt_icap = c;
}

/* a new register like `like` (its type and, for the dump, its name) */
static int opt_vreg(IRType t, int like) {
    int v = ir_vreg(opt_F, t);
    if (like >= 0) {
        opt_F->vregs[v].name = opt_F->vregs[like].name;
    }
    opt_vroom();
    opt_def[v] = -1;
    opt_repl[v] = -1;
    opt_nuse[v] = 0;
    opt_user[v] = -1;
    opt_vstamp[v] = -1;
    opt_vaux[v] = -1;
    opt_vflag[v] = 0;
    return v;
}

static IRType opt_vtype(int v) {
    return (IRType)opt_F->vregs[v].type;
}

/* a register of a type the optimizer works on */
static int opt_is_var(int v) {
    IRType t = opt_vtype(v);
    return t == IR_I32 || t == IR_I64 || t == IR_F32 || t == IR_F64;
}

static int opt_is_int(int v) {
    IRType t = opt_vtype(v);
    return t == IR_I32 || t == IR_I64;
}

static int opt_inst(const IRInst *proto) {
    opt_I = opt_grow(opt_I, &opt_capi, opt_ni + 1, sizeof(IRInst));
    opt_I[opt_ni] = *proto;
    opt_iroom();
    opt_blk[opt_ni] = -1;
    opt_pos[opt_ni] = -1;
    opt_mark[opt_ni] = 0;
    return opt_ni++;
}

static int opt_block_new(int after) {
    OptBlock *b;
    opt_B = opt_grow(opt_B, &opt_capb, opt_nb + 1, sizeof(OptBlock));
    b = &opt_B[opt_nb];
    memset(b, 0, sizeof(*b));
    b->live = 1;
    b->after = after;
    b->idom = -1;
    b->rpo = -1;
    return opt_nb++;
}

static void opt_push(int b, int inst) {
    OptBlock *k = &opt_B[b];
    k->code = opt_grow(k->code, &k->cap, k->n + 1, sizeof(int));
    k->code[k->n++] = inst;
}

/* insert `inst` at index `at` of block b */
static void opt_insert(int b, int at, int inst) {
    OptBlock *k = &opt_B[b];
    k->code = opt_grow(k->code, &k->cap, k->n + 1, sizeof(int));
    memmove(&k->code[at + 1], &k->code[at], (size_t)(k->n - at) * sizeof(int));
    k->code[at] = inst;
    k->n++;
}

static void opt_add_pred(int b, int p) {
    OptBlock *k = &opt_B[b];
    k->preds = opt_grow(k->preds, &k->pcap, k->npreds + 1, sizeof(int));
    k->preds[k->npreds++] = p;
}

static IRInst *opt_term(int b) {
    return &opt_I[opt_B[b].code[opt_B[b].n - 1]];
}

/* the successors of block b, with repeats (a switch can go to one block from several entries) */
static int opt_nsucc(int b) {
    if (!opt_B[b].live || opt_B[b].n == 0) {
        return 0;
    }
    return ir_nsuccs(opt_F, opt_term(b));
}

static int opt_succ(int b, int k) {
    return ir_succ(opt_F, opt_term(b), k);
}

/* successor k of b is the first edge from b to its block */
static int opt_first_edge(int b, int k) {
    int j, s = opt_succ(b, k);
    for (j = 0; j < k; j++) {
        if (opt_succ(b, j) == s) {
            return 0;
        }
    }
    return 1;
}

/* the operands of an instruction, as pointers so they can be rewritten; unused ones hold -1 */
static int opt_nops(const IRInst *i) {
    if (i->op == OPT_PHI) {
        return opt_P[i->imm].n;
    }
    if (i->op == IR_CALL) {
        return 5 + opt_F->calls[i->imm].nargs;
    }
    return 2;
}

static int *opt_op(IRInst *i, int k) {
    IRCall *c;
    if (i->op == OPT_PHI) {
        return &opt_P[i->imm].v[k];
    }
    if (k == 0) {
        return &i->a;
    }
    if (k == 1) {
        return &i->b;
    }
    c = &opt_F->calls[i->imm];
    if (k == 2) {
        return &c->target;
    }
    if (k == 3) {
        return &c->sret_addr;
    }
    if (k == 4) {
        return &c->result_addr;
    }
    return &opt_F->args[c->first_arg + k - 5].v;
}

static int opt_is_undef(int v) {
    int d = v >= 0 ? opt_def[v] : -1;
    return d >= 0 && (opt_I[d].op == IR_ICONST || opt_I[d].op == IR_FCONST) && opt_I[d].aux == OPT_UNDEF;
}

static int opt_resolve(int v) {
    while (v >= 0 && opt_repl[v] >= 0) {
        v = opt_repl[v];
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* the CFG                                                            */
/* ------------------------------------------------------------------ */

/* drop predecessor p of block s (its first occurrence) and the phi arguments for it */
static void opt_remove_pred(int s, int p) {
    OptBlock *b = &opt_B[s];
    int k, j;
    for (k = 0; k < b->npreds && b->preds[k] != p; k++) {
    }
    if (k == b->npreds) {
        return;
    }
    memmove(&b->preds[k], &b->preds[k + 1], (size_t)(b->npreds - k - 1) * sizeof(int));
    b->npreds--;
    for (j = 0; j < b->n; j++) {
        IRInst *i = &opt_I[b->code[j]];
        OptPhi *ph;
        if (i->op != OPT_PHI) {
            break;
        }
        ph = &opt_P[i->imm];
        memmove(&ph->v[k], &ph->v[k + 1], (size_t)(ph->n - k - 1) * sizeof(int));
        ph->n--;
    }
}

static void opt_compute_preds(void) {
    int b, k, n;
    for (b = 0; b < opt_nb; b++) {
        opt_B[b].npreds = 0;
    }
    for (b = 0; b < opt_nb; b++) {
        n = opt_nsucc(b);
        for (k = 0; k < n; k++) {
            opt_add_pred(opt_succ(b, k), b);
        }
    }
}

/* drop the blocks the entry no longer reaches; returns 1 if there were any */
static int opt_prune(void) {
    int *stack = xmalloc((size_t)(opt_nb + 1) * sizeof(int));
    char *seen = xcalloc((size_t)opt_nb + 1, 1);
    int sp = 0;
    int b, k, n, any = 0;
    stack[sp++] = 0;
    seen[0] = 1;
    while (sp) {
        b = stack[--sp];
        n = opt_nsucc(b);
        for (k = 0; k < n; k++) {
            int s = opt_succ(b, k);
            if (!seen[s]) {
                seen[s] = 1;
                stack[sp++] = s;
            }
        }
    }
    for (b = 0; b < opt_nb; b++) {
        if (opt_B[b].live && !seen[b]) {
            n = opt_nsucc(b);
            for (k = 0; k < n; k++) {
                opt_remove_pred(opt_succ(b, k), b);
            }
            opt_B[b].live = 0;
            opt_B[b].n = 0;
            any = 1;
        }
    }
    free(stack);
    free(seen);
    return any;
}

static int opt_dom_intersect(int a, int b) {
    while (a != b) {
        while (opt_B[a].rpo > opt_B[b].rpo) {
            a = opt_B[a].idom;
        }
        while (opt_B[b].rpo > opt_B[a].rpo) {
            b = opt_B[b].idom;
        }
    }
    return a;
}

/* reverse postorder, immediate dominators, the dominator tree and its numbering */
static void opt_dominators(void) {
    int *stack = xmalloc((size_t)(2 * opt_nb + 2) * sizeof(int));
    int *post = xmalloc((size_t)(opt_nb + 1) * sizeof(int));
    int *last = xmalloc((size_t)(opt_nb + 1) * sizeof(int));
    char *seen = xcalloc((size_t)opt_nb + 1, 1);
    int sp = 0, np = 0, changed, b, k, i, n, counter;

    for (b = 0; b < opt_nb; b++) {
        opt_B[b].rpo = -1;
        opt_B[b].idom = -1;
        opt_B[b].child = -1;
        opt_B[b].sibling = -1;
    }
    /* postorder by an iterative depth-first search: each entry is a block and the successors it has visited */
    stack[sp++] = 0;
    stack[sp++] = 0;
    seen[0] = 1;
    while (sp) {
        int next = stack[sp - 1];
        b = stack[sp - 2];
        n = opt_nsucc(b);
        if (next < n) {
            int s = opt_succ(b, next);
            stack[sp - 1] = next + 1;
            if (!seen[s]) {
                seen[s] = 1;
                stack[sp++] = s;
                stack[sp++] = 0;
            }
        } else {
            post[np++] = b;
            sp -= 2;
        }
    }
    opt_rpo = opt_grow(opt_rpo, &opt_caprpo, np + 1, sizeof(int));
    opt_nrpo = np;
    for (k = 0; k < np; k++) {
        opt_rpo[k] = post[np - 1 - k];
        opt_B[opt_rpo[k]].rpo = k;
    }
    opt_B[0].idom = 0;
    do {
        changed = 0;
        for (k = 1; k < np; k++) {
            int nd = -1;
            b = opt_rpo[k];
            for (i = 0; i < opt_B[b].npreds; i++) {
                int p = opt_B[b].preds[i];
                if (opt_B[p].idom < 0) {
                    continue;
                }
                nd = nd < 0 ? p : opt_dom_intersect(p, nd);
            }
            if (nd != opt_B[b].idom) {
                opt_B[b].idom = nd;
                changed = 1;
            }
        }
    } while (changed);
    /* children in reverse postorder */
    for (k = 0; k < np; k++) {
        last[opt_rpo[k]] = -1;
    }
    for (k = 1; k < np; k++) {
        int d;
        b = opt_rpo[k];
        d = opt_B[b].idom;
        if (last[d] < 0) {
            opt_B[d].child = b;
        } else {
            opt_B[last[d]].sibling = b;
        }
        last[d] = b;
    }
    /* preorder and postorder numbers of the tree: each stack entry is a block and the child to visit next */
    counter = 0;
    sp = 0;
    stack[sp++] = 0;
    stack[sp++] = opt_B[0].child;
    opt_B[0].pre = counter++;
    while (sp) {
        int c = stack[sp - 1];
        b = stack[sp - 2];
        if (c >= 0) {
            stack[sp - 1] = opt_B[c].sibling;
            opt_B[c].pre = counter++;
            stack[sp++] = c;
            stack[sp++] = opt_B[c].child;
        } else {
            opt_B[b].post = counter++;
            sp -= 2;
        }
    }
    free(stack);
    free(post);
    free(last);
    free(seen);
}

static int opt_dominates(int a, int b) {
    return opt_B[a].pre <= opt_B[b].pre && opt_B[b].post <= opt_B[a].post;
}

static void opt_frontiers(void) {
    int k, i;
    for (k = 0; k < opt_nb; k++) {
        opt_B[k].ndf = 0;
    }
    for (k = 0; k < opt_nrpo; k++) {
        int b = opt_rpo[k];
        if (opt_B[b].npreds < 2) {
            continue;
        }
        for (i = 0; i < opt_B[b].npreds; i++) {
            int r = opt_B[b].preds[i];
            while (r != opt_B[b].idom) {
                OptBlock *rb = &opt_B[r];
                if (rb->ndf == 0 || rb->df[rb->ndf - 1] != b) {
                    rb->df = opt_grow(rb->df, &rb->dfcap, rb->ndf + 1, sizeof(int));
                    rb->df[rb->ndf++] = b;
                }
                r = rb->idom;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* loading and storing the function                                   */
/* ------------------------------------------------------------------ */

static void opt_load(void) {
    int b, i;
    opt_ni = opt_nb = opt_np = 0;
    for (i = 0; i < opt_F->ninsts; i++) {
        opt_inst(&opt_F->insts[i]);
    }
    for (b = 0; b < opt_F->nblocks; b++) {
        const IRBlock *blk = &opt_F->blocks[b];
        int nb = opt_block_new(-1);
        opt_B[nb].order = blk->start;
        if (blk->start < 0 || blk->end <= blk->start) {
            opt_B[nb].live = 0;
            continue;
        }
        for (i = blk->start; i < blk->end; i++) {
            opt_push(nb, i);
        }
    }
    for (b = 0; b < opt_nb; b++) {
        IRInst *t;
        if (!opt_B[b].live) {
            continue;
        }
        t = opt_term(b);
        if (t->op == IR_CONDBR && t->imm == t->aux) {
            t->op = IR_BR;
            t->a = -1;
            t->type = IR_VOID;
        }
    }
    opt_prune();
    opt_compute_preds();
}

static int opt_by_order(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    if (opt_B[x].order != opt_B[y].order) {
        return opt_B[x].order < opt_B[y].order ? -1 : 1;
    }
    return x - y;
}

/*
 * The order blocks are emitted in: the function's own in the order of
 * their code (a switch's case blocks are made before its body is
 * lowered, so their numbers say nothing about where they belong), each
 * followed by the blocks the optimizer made after it.
 */
static int opt_layout(int *order) {
    int *first = xmalloc((size_t)(opt_nb + 1) * sizeof(int));
    int *next = xmalloc((size_t)(opt_nb + 1) * sizeof(int));
    int *tail = xmalloc((size_t)(opt_nb + 1) * sizeof(int));
    int *stack = xmalloc((size_t)(opt_nb + 1) * sizeof(int));
    int b, n = 0, sp = 0;
    for (b = 0; b < opt_nb; b++) {
        first[b] = next[b] = tail[b] = -1;
    }
    for (b = 0; b < opt_nb; b++) {
        int a = opt_B[b].after;
        if (a < 0) {
            continue;
        }
        if (tail[a] < 0) {
            first[a] = b;
        } else {
            next[tail[a]] = b;
        }
        tail[a] = b;
    }
    for (b = 0; b < opt_nb; b++) {
        if (opt_B[b].after < 0) {
            stack[sp++] = b;
        }
    }
    /* sorted, then reversed onto the stack so that the first is on top */
    qsort(stack, (size_t)sp, sizeof(int), opt_by_order);
    for (b = 0; b < sp / 2; b++) {
        int t = stack[b];
        stack[b] = stack[sp - 1 - b];
        stack[sp - 1 - b] = t;
    }
    while (sp) {
        int c, m, k;
        b = stack[--sp];
        if (opt_B[b].live) {
            order[n++] = b;
        }
        /* its children, first one on top */
        m = 0;
        for (c = first[b]; c >= 0; c = next[c]) {
            m++;
        }
        sp += m;
        k = sp - 1;
        for (c = first[b]; c >= 0; c = next[c]) {
            stack[k--] = c;
        }
    }
    free(first);
    free(next);
    free(tail);
    free(stack);
    return n;
}

static void opt_store(void) {
    int *order = xmalloc((size_t)(opt_nb + 1) * sizeof(int));
    int *newid = xmalloc((size_t)(opt_nb + 1) * sizeof(int));
    int n = opt_layout(order);
    int k, j, b, ni = 0;
    IRInst *insts;
    for (b = 0; b < opt_nb; b++) {
        newid[b] = -1;
    }
    for (k = 0; k < n; k++) {
        newid[order[k]] = k;
        for (j = 0; j < opt_B[order[k]].n; j++) {
            if (opt_I[opt_B[order[k]].code[j]].op != IR_NOP) {
                ni++;
            }
        }
    }
    insts = xmalloc((size_t)(ni + 1) * sizeof(IRInst));
    opt_F->blocks = opt_grow(opt_F->blocks, &opt_F->blocks_cap, n + 1, sizeof(IRBlock));
    ni = 0;
    for (k = 0; k < n; k++) {
        OptBlock *ob = &opt_B[order[k]];
        opt_F->blocks[k].start = ni;
        for (j = 0; j < ob->n; j++) {
            IRInst *i = &opt_I[ob->code[j]];
            if (i->op == IR_NOP) {
                continue;
            }
            insts[ni] = *i;
            if (ir_is_terminator((IROp)i->op)) {
                int e;
                for (e = 0; e < ir_nsuccs(opt_F, i); e++) {
                    ir_set_succ(opt_F, &insts[ni], e, newid[ir_succ(opt_F, i, e)]);
                }
            }
            ni++;
        }
        opt_F->blocks[k].end = ni;
    }
    free(opt_F->insts);
    opt_F->insts = insts;
    opt_F->ninsts = ni;
    opt_F->insts_cap = ni + 1;
    opt_F->nblocks = n;
    free(order);
    free(newid);
}

/* ------------------------------------------------------------------ */
/* SSA construction                                                   */
/* ------------------------------------------------------------------ */

static int opt_phi_new(int b, int var) {
    IRInst proto;
    int k, idx;
    memset(&proto, 0, sizeof(proto));
    proto.op = OPT_PHI;
    proto.type = (unsigned char)opt_vtype(var);
    proto.dst = var;
    proto.a = proto.b = -1;
    proto.aux = var;
    proto.imm = opt_np;
    opt_P = opt_grow(opt_P, &opt_capp, opt_np + 1, sizeof(OptPhi));
    opt_P[opt_np].n = opt_B[b].npreds;
    opt_P[opt_np].v = xmalloc((size_t)(opt_B[b].npreds + 1) * sizeof(int));
    for (k = 0; k < opt_B[b].npreds; k++) {
        opt_P[opt_np].v[k] = -1;
    }
    opt_np++;
    idx = opt_inst(&proto);
    opt_insert(b, 0, idx);
    return idx;
}

/* the renaming's stacks: per register the top entry, entries chained to the one below */
static int *opt_top = NULL;
static int *opt_sval = NULL;
static int *opt_sprev = NULL;
static int opt_ns = 0;
static int opt_caps = 0;
static int *opt_log = NULL;
static int opt_nlog = 0;
static int opt_caplog = 0;
static int opt_undef[IR_F80 + 1];

static void opt_spush(int var, int v) {
    opt_sval = opt_grow(opt_sval, &opt_caps, opt_ns + 1, sizeof(int));
    opt_sprev = xrealloc(opt_sprev, (size_t)opt_caps * sizeof(int));
    opt_sval[opt_ns] = v;
    opt_sprev[opt_ns] = opt_top[var];
    opt_top[var] = opt_ns++;
    opt_log = opt_grow(opt_log, &opt_caplog, opt_nlog + 1, sizeof(int));
    opt_log[opt_nlog++] = var;
}

static int opt_current(int var) {
    return opt_top[var] >= 0 ? opt_sval[opt_top[var]] : opt_undef[opt_vtype(var)];
}

/* the name of a new definition of `var`: the first is the register itself, the rest new ones */
static int opt_newname(int var) {
    if (!opt_vflag[var]) {
        opt_vflag[var] = 1;
        return var;
    }
    return opt_vreg(opt_vtype(var), var);
}

static int opt_renamed(int v) {
    return v >= 0 && v < opt_nvars && opt_is_var(v);
}

static void opt_rename_block(int b) {
    OptBlock *ob = &opt_B[b];
    int j, k, n, i2, s2;
    for (j = 0; j < ob->n; j++) {
        IRInst *i = &opt_I[ob->code[j]];
        if (i->op == OPT_PHI) {
            i->dst = opt_newname(i->aux);
            opt_spush(i->aux, i->dst);
            continue;
        }
        n = opt_nops(i);
        for (k = 0; k < n; k++) {
            int *p = opt_op(i, k);
            if (opt_renamed(*p)) {
                *p = opt_current(*p);
            }
        }
        if (opt_renamed(i->dst)) {
            int var = i->dst;
            i->dst = opt_newname(var);
            opt_spush(var, i->dst);
        }
    }
    n = opt_nsucc(b);
    for (k = 0; k < n; k++) {
        OptBlock *sb;
        if (!opt_first_edge(b, k)) {
            continue;
        }
        sb = &opt_B[opt_succ(b, k)];
        for (s2 = 0; s2 < sb->npreds; s2++) {
            if (sb->preds[s2] != b) {
                continue;
            }
            for (i2 = 0; i2 < sb->n; i2++) {
                IRInst *phi = &opt_I[sb->code[i2]];
                if (phi->op != OPT_PHI) {
                    break;
                }
                opt_P[phi->imm].v[s2] = opt_current(phi->aux);
            }
        }
    }
}

static void opt_build_ssa(void) {
    int nv = opt_F->nvregs;
    int *dhead, *dnext, *dblock, *work, *hasphi, *inwork, *mark, *stack;
    int nd = 0, capd = 0, k, b, j, v, sp;
    char *global;
    static const IRType types[4] = { IR_I32, IR_I64, IR_F32, IR_F64 };

    opt_nvars = nv;
    opt_vroom();
    dhead = xmalloc((size_t)(nv + 1) * sizeof(int));
    global = xcalloc((size_t)nv + 1, 1);
    dnext = NULL;
    dblock = NULL;
    for (v = 0; v < nv; v++) {
        dhead[v] = -1;
        opt_vstamp[v] = -1;
    }
    /* registers read in a block before it writes them, and the blocks writing each */
    for (k = 0; k < opt_nrpo; k++) {
        b = opt_rpo[k];
        for (j = 0; j < opt_B[b].n; j++) {
            IRInst *i = &opt_I[opt_B[b].code[j]];
            int n = opt_nops(i), m;
            for (m = 0; m < n; m++) {
                int u = *opt_op(i, m);
                if (opt_renamed(u) && opt_vstamp[u] != b) {
                    global[u] = 1;
                }
            }
            if (opt_renamed(i->dst)) {
                v = i->dst;
                if (opt_vstamp[v] != b) {
                    int c2 = capd;
                    opt_vstamp[v] = b;
                    dnext = opt_grow(dnext, &capd, nd + 1, sizeof(int));
                    dblock = opt_grow(dblock, &c2, nd + 1, sizeof(int));
                    dnext[nd] = dhead[v];
                    dblock[nd] = b;
                    dhead[v] = nd++;
                }
            }
        }
    }
    /* phis at the iterated dominance frontiers */
    opt_frontiers();
    work = xmalloc((size_t)(opt_nb + 1) * sizeof(int));
    hasphi = xmalloc((size_t)(opt_nb + 1) * sizeof(int));
    inwork = xmalloc((size_t)(opt_nb + 1) * sizeof(int));
    for (b = 0; b < opt_nb; b++) {
        hasphi[b] = inwork[b] = -1;
    }
    for (v = 0; v < nv; v++) {
        int nw = 0, d;
        if (!global[v] || dhead[v] < 0) {
            continue;
        }
        for (d = dhead[v]; d >= 0; d = dnext[d]) {
            work[nw++] = dblock[d];
            inwork[dblock[d]] = v;
        }
        while (nw) {
            int x = work[--nw];
            for (k = 0; k < opt_B[x].ndf; k++) {
                int y = opt_B[x].df[k];
                if (hasphi[y] == v) {
                    continue;
                }
                hasphi[y] = v;
                opt_phi_new(y, v);
                if (inwork[y] != v) {
                    inwork[y] = v;
                    work[nw++] = y;
                }
            }
        }
    }
    free(dhead);
    free(dnext);
    free(dblock);
    free(global);
    free(work);
    free(hasphi);
    free(inwork);

    /* the value of a register read before any write: zero, defined at the entry, and marked as undefined */
    for (k = 0; k < 4; k++) {
        IRInst proto;
        memset(&proto, 0, sizeof(proto));
        proto.op = ir_type_is_float(types[k]) ? IR_FCONST : IR_ICONST;
        proto.type = (unsigned char)types[k];
        proto.dst = opt_vreg(types[k], -1);
        proto.a = proto.b = -1;
        proto.aux = OPT_UNDEF;
        opt_undef[types[k]] = proto.dst;
        opt_insert(0, 0, opt_inst(&proto));
    }

    /* renaming, along the dominator tree */
    opt_top = xrealloc(opt_top, (size_t)(nv + 1) * sizeof(int));
    for (v = 0; v < nv; v++) {
        opt_top[v] = -1;
        opt_vflag[v] = 0;
    }
    opt_ns = 0;
    opt_nlog = 0;
    for (k = 0; k < opt_F->nparams; k++) {
        v = opt_F->params[k].vreg;
        if (v >= 0 && opt_renamed(v)) {
            opt_vflag[v] = 1;
            opt_spush(v, v);
        }
    }
    mark = xmalloc((size_t)(opt_nb + 1) * sizeof(int));
    stack = xmalloc((size_t)(2 * opt_nb + 2) * sizeof(int));
    sp = 0;
    stack[sp++] = 0;
    while (sp) {
        int e = stack[--sp];
        int c;
        b = e >> 1;
        if (e & 1) {
            while (opt_nlog > mark[b]) {
                v = opt_log[--opt_nlog];
                opt_top[v] = opt_sprev[opt_top[v]];
            }
            continue;
        }
        mark[b] = opt_nlog;
        stack[sp++] = b * 2 + 1;
        opt_rename_block(b);
        for (c = opt_B[b].child; c >= 0; c = opt_B[c].sibling) {
            stack[sp++] = c * 2;
        }
    }
    free(mark);
    free(stack);
    opt_vroom();
}

/* ------------------------------------------------------------------ */
/* uses and definitions                                               */
/* ------------------------------------------------------------------ */

/* opt_def, opt_nuse, opt_user, and each instruction's block and position */
static void opt_index(void) {
    int v, k, j, n, m;
    opt_vroom();
    opt_iroom();
    for (v = 0; v < opt_F->nvregs; v++) {
        opt_def[v] = -1;
        opt_nuse[v] = 0;
        opt_user[v] = -1;
    }
    for (k = 0; k < opt_nrpo; k++) {
        int b = opt_rpo[k];
        for (j = 0; j < opt_B[b].n; j++) {
            int ii = opt_B[b].code[j];
            IRInst *i = &opt_I[ii];
            opt_blk[ii] = b;
            opt_pos[ii] = j;
            if (i->op == IR_NOP) {
                continue;
            }
            n = opt_nops(i);
            for (m = 0; m < n; m++) {
                int u = *opt_op(i, m);
                if (u >= 0) {
                    opt_nuse[u]++;
                    opt_user[u] = ii;
                }
            }
            if (i->dst >= 0) {
                opt_def[i->dst] = ii;
            }
        }
    }
}

/* remove the instructions turned into nops from the blocks */
static void opt_compact(void) {
    int b, j, n;
    for (b = 0; b < opt_nb; b++) {
        OptBlock *ob = &opt_B[b];
        n = 0;
        for (j = 0; j < ob->n; j++) {
            if (opt_I[ob->code[j]].op != IR_NOP) {
                ob->code[n++] = ob->code[j];
            }
        }
        ob->n = n;
    }
}

static void opt_kill(IRInst *i) {
    i->op = IR_NOP;
    i->dst = -1;
    i->a = i->b = -1;
    i->flags = 0;
}

/* rewrite every operand through opt_repl */
static void opt_apply_repl(void) {
    int b, j, k, n;
    for (b = 0; b < opt_nb; b++) {
        if (!opt_B[b].live) {
            continue;
        }
        for (j = 0; j < opt_B[b].n; j++) {
            IRInst *i = &opt_I[opt_B[b].code[j]];
            if (i->op == IR_NOP) {
                continue;
            }
            n = opt_nops(i);
            for (k = 0; k < n; k++) {
                int *p = opt_op(i, k);
                if (*p >= 0) {
                    *p = opt_resolve(*p);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* folding and simplification                                         */
/* ------------------------------------------------------------------ */

static long opt_fit(IRType t, long v) {
    return t == IR_I32 ? (long)(int)v : v;
}

static unsigned long opt_unsigned(IRType t, long v) {
    return t == IR_I32 ? (unsigned long)(unsigned)v : (unsigned long)v;
}

static int opt_const(int v, long *out) {
    int d;
    if (v < 0) {
        return 0;
    }
    d = opt_def[v];
    if (d < 0 || opt_I[d].op != IR_ICONST) {
        return 0;
    }
    *out = opt_I[d].imm;
    return 1;
}

static int opt_log2(unsigned long u) {
    int k = 0;
    if (u == 0 || (u & (u - 1)) != 0) {
        return -1;
    }
    while (u > 1) {
        u >>= 1;
        k++;
    }
    return k;
}

static int opt_fold_bin(IROp op, IRType t, long x, long y, long *r) {
    unsigned long ux = opt_unsigned(t, x);
    unsigned long uy = opt_unsigned(t, y);
    int sh = (int)(y & (t == IR_I32 ? 31 : 63));
    long mn = t == IR_I32 ? (long)INT_MIN : LONG_MIN;
    x = opt_fit(t, x);
    y = opt_fit(t, y);
    switch (op) {
        case IR_ADD: *r = (long)((unsigned long)x + (unsigned long)y); break;
        case IR_SUB: *r = (long)((unsigned long)x - (unsigned long)y); break;
        case IR_MUL: *r = (long)((unsigned long)x * (unsigned long)y); break;
        case IR_SDIV:
            if (y == 0 || (x == mn && y == -1)) {
                return 0;
            }
            *r = x / y;
            break;
        case IR_SREM:
            if (y == 0 || (x == mn && y == -1)) {
                return 0;
            }
            *r = x % y;
            break;
        case IR_UDIV:
            if (uy == 0) {
                return 0;
            }
            *r = (long)(ux / uy);
            break;
        case IR_UREM:
            if (uy == 0) {
                return 0;
            }
            *r = (long)(ux % uy);
            break;
        case IR_SHL: *r = (long)(ux << sh); break;
        case IR_LSHR: *r = (long)(ux >> sh); break;
        case IR_ASHR: *r = x >> sh; break;
        case IR_AND: *r = x & y; break;
        case IR_OR: *r = x | y; break;
        case IR_XOR: *r = x ^ y; break;
        default: return 0;
    }
    *r = opt_fit(t, *r);
    return 1;
}

static int opt_fold_cmp(IRCond cc, IRType t, long x, long y) {
    unsigned long ux = opt_unsigned(t, x);
    unsigned long uy = opt_unsigned(t, y);
    x = opt_fit(t, x);
    y = opt_fit(t, y);
    switch (cc) {
        case IR_EQ: return x == y;
        case IR_NE: return x != y;
        case IR_LT: return x < y;
        case IR_GE: return x >= y;
        case IR_GT: return x > y;
        case IR_LE: return x <= y;
        case IR_ULT: return ux < uy;
        case IR_UGE: return ux >= uy;
        case IR_UGT: return ux > uy;
        case IR_ULE: return ux <= uy;
    }
    return 0;
}

static IRCond opt_swap_cond(IRCond cc) {
    switch (cc) {
        case IR_LT: return IR_GT;
        case IR_GT: return IR_LT;
        case IR_LE: return IR_GE;
        case IR_GE: return IR_LE;
        case IR_ULT: return IR_UGT;
        case IR_UGT: return IR_ULT;
        case IR_ULE: return IR_UGE;
        case IR_UGE: return IR_ULE;
        default: return cc;
    }
}

static IRCond opt_invert_cond(IRCond cc) {
    switch (cc) {
        case IR_EQ: return IR_NE;
        case IR_NE: return IR_EQ;
        case IR_LT: return IR_GE;
        case IR_GE: return IR_LT;
        case IR_GT: return IR_LE;
        case IR_LE: return IR_GT;
        case IR_ULT: return IR_UGE;
        case IR_UGE: return IR_ULT;
        case IR_UGT: return IR_ULE;
        case IR_ULE: return IR_UGT;
    }
    return cc;
}

static int opt_commutative(IROp op) {
    return op == IR_ADD || op == IR_MUL || op == IR_AND || op == IR_OR || op == IR_XOR || op == IR_FADD || op == IR_FMUL;
}

static int opt_int_binop(IROp op) {
    return op >= IR_ADD && op <= IR_XOR;
}

static void opt_make_const(IRInst *i, long v) {
    IRType t = opt_vtype(i->dst);
    i->op = IR_ICONST;
    i->type = (unsigned char)t;
    i->a = i->b = -1;
    i->flags = 0;
    i->cc = 0;
    i->size = 0;
    i->imm = opt_fit(t, v);
}

/* the bits of an integer register that may be set (the low 32 for an i32) */
static unsigned long opt_maybe_set(int v, int depth) {
    IRType t = opt_vtype(v);
    unsigned long all = t == IR_I32 ? 0xffffffffUL : ~0UL;
    const IRInst *d;
    if (opt_def[v] < 0 || depth > 6) {
        return all;
    }
    d = &opt_I[opt_def[v]];
    switch (d->op) {
        case IR_ICONST:
            return (unsigned long)d->imm & all;
        case IR_LOAD:
            if (!(d->flags & IRF_SIGNED) && d->size < ir_type_size(t)) {
                return (1UL << (8 * d->size)) - 1;
            }
            return all;
        case IR_LOADBITS:
            if (!(d->flags & IRF_SIGNED) && (d->imm >> 8) < 32) {
                return (1UL << (d->imm >> 8)) - 1;
            }
            return all;
        case IR_ZEXT:
            if (d->size < 8) {
                return ((1UL << (8 * d->size)) - 1) & all;
            }
            return all;
        case IR_ICMP:
        case IR_FCMP:
            return 1;
        case IR_AND:
            if (d->flags & IRF_IMM) {
                return opt_maybe_set(d->a, depth + 1) & (unsigned long)d->imm & all;
            }
            return opt_maybe_set(d->a, depth + 1) & opt_maybe_set(d->b, depth + 1);
        case IR_OR:
        case IR_XOR:
            if (d->flags & IRF_IMM) {
                return (opt_maybe_set(d->a, depth + 1) | (unsigned long)d->imm) & all;
            }
            return opt_maybe_set(d->a, depth + 1) | opt_maybe_set(d->b, depth + 1);
        case IR_LSHR:
            if (d->flags & IRF_IMM) {
                return (opt_maybe_set(d->a, depth + 1) & all) >> (d->imm & (t == IR_I32 ? 31 : 63));
            }
            return all;
        case IR_SHL:
            if (d->flags & IRF_IMM) {
                return (opt_maybe_set(d->a, depth + 1) << (d->imm & (t == IR_I32 ? 31 : 63))) & all;
            }
            return all;
        default:
            return all;
    }
}

/* the value of a floating point constant register, as a double */
static int opt_fconst(int v, double *out) {
    const IRInst *d;
    if (v < 0 || opt_def[v] < 0) {
        return 0;
    }
    d = &opt_I[opt_def[v]];
    if (d->op != IR_FCONST || d->aux == OPT_UNDEF) {
        return 0;
    }
    if (d->type == IR_F32) {
        float f;
        unsigned u = (unsigned)d->imm;
        memcpy(&f, &u, sizeof(f));
        *out = f;
    } else if (d->type == IR_F64) {
        memcpy(out, &d->imm, sizeof(*out));
    } else {
        return 0;
    }
    return 1;
}

static void opt_make_fconst(IRInst *i, double v) {
    IRType t = opt_vtype(i->dst);
    i->op = IR_FCONST;
    i->type = (unsigned char)t;
    i->a = i->b = -1;
    i->flags = 0;
    i->cc = 0;
    i->size = 0;
    i->t2 = 0;
    i->aux = -1;
    if (t == IR_F32) {
        float f = (float)v;
        unsigned u;
        memcpy(&u, &f, sizeof(u));
        i->imm = (long)u;
    } else {
        memcpy(&i->imm, &v, sizeof(i->imm));
    }
}

/*
 * Floating point arithmetic on constants, as the target does it at run
 * time in the default rounding mode (both targets are IEEE 754, and so is
 * every host kcc runs on): float operations in float, double in double.
 * Integer conversions out of range are undefined and are left alone.
 */
static int opt_fold_float(IRInst *i) {
    double x, y;
    long n;
    IRType t = (IRType)i->type;
    if (t != IR_F32 && t != IR_F64 && i->op != IR_FTOI && i->op != IR_FCMP) {
        return 0;
    }
    switch (i->op) {
        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV:
            if (!opt_fconst(i->a, &x) || !opt_fconst(i->b, &y)) {
                return 0;
            }
            if (t == IR_F32) {
                float a = (float)x, b = (float)y, r;
                r = i->op == IR_FADD ? a + b : i->op == IR_FSUB ? a - b : i->op == IR_FMUL ? a * b : a / b;
                opt_make_fconst(i, r);
            } else {
                opt_make_fconst(i, i->op == IR_FADD ? x + y : i->op == IR_FSUB ? x - y : i->op == IR_FMUL ? x * y : x / y);
            }
            return 1;
        case IR_FNEG:
            if (!opt_fconst(i->a, &x)) {
                return 0;
            }
            opt_make_fconst(i, -x);
            return 1;
        case IR_FABS:
            if (!opt_fconst(i->a, &x)) {
                return 0;
            }
            opt_make_fconst(i, x < 0 || (x == 0 && 1 / x < 0) ? -x : x);
            return 1;
        case IR_FTOF:
            if (!opt_fconst(i->a, &x)) {
                return 0;
            }
            opt_make_fconst(i, x);
            return 1;
        case IR_ITOF:
            if (!opt_const(i->a, &n) || (i->t2 != IR_I32 && i->t2 != IR_I64)) {
                return 0;
            }
            if (i->flags & IRF_SIGNED) {
                x = i->t2 == IR_I32 ? (double)(int)n : (double)n;
            } else {
                x = i->t2 == IR_I32 ? (double)(unsigned)n : (double)(unsigned long)n;
            }
            if (t == IR_F32) {
                /* straight to float: through double could round twice */
                float f = (i->flags & IRF_SIGNED) ? (i->t2 == IR_I32 ? (float)(int)n : (float)n)
                                                  : (i->t2 == IR_I32 ? (float)(unsigned)n : (float)(unsigned long)n);
                x = f;
            }
            opt_make_fconst(i, x);
            return 1;
        case IR_FCMP:
            if (!opt_fconst(i->a, &x) || !opt_fconst(i->b, &y)) {
                return 0;
            }
            switch ((IRCond)i->cc) {
                case IR_EQ: n = x == y; break;
                case IR_NE: n = x != y; break;
                case IR_LT: n = x < y; break;
                case IR_LE: n = x <= y; break;
                case IR_GT: n = x > y; break;
                case IR_GE: n = x >= y; break;
                default: return 0;
            }
            opt_make_const(i, n);
            return 1;
        default:
            return 0;
    }
}

/* the operands and destination are all registers the optimizer works on */
static int opt_all_vars(const IRInst *i) {
    if (i->dst < 0 || !opt_is_var(i->dst)) {
        return 0;
    }
    if (i->a >= 0 && !opt_is_var(i->a)) {
        return 0;
    }
    if (i->b >= 0 && !opt_is_var(i->b)) {
        return 0;
    }
    return 1;
}

/*
 * Simplify one instruction. Returns a register its result equals (the
 * caller replaces it), or -1 when it stays, possibly rewritten into a
 * simpler form.
 */
static int opt_simplify(IRInst *i) {
    IRType t = (IRType)i->type;
    IROp op = (IROp)i->op;
    long x = 0, y = 0, r;
    int ca, cb;
    if (!opt_all_vars(i)) {
        return -1;
    }
    if (op == IR_MOV) {
        return opt_vtype(i->a) == opt_vtype(i->dst) ? i->a : -1;
    }
    if (opt_int_binop(op)) {
        int bits = t == IR_I32 ? 32 : 64;
        unsigned long all = t == IR_I32 ? 0xffffffffUL : ~0UL;
        unsigned long uy;
        int k;
        if (t != IR_I32 && t != IR_I64) {
            return -1;
        }
        ca = opt_const(i->a, &x);
        if (i->flags & IRF_IMM) {
            cb = 1;
            y = i->imm;
        } else {
            cb = opt_const(i->b, &y);
        }
        if (ca && cb && opt_fold_bin(op, t, x, y, &r)) {
            opt_make_const(i, r);
            return -1;
        }
        if (ca && !cb && opt_commutative(op)) {
            i->a = i->b;
            i->b = -1;
            i->flags |= IRF_IMM;
            i->imm = opt_fit(t, x);
            cb = 1;
            y = x;
        } else if (cb && !(i->flags & IRF_IMM)) {
            i->b = -1;
            i->flags |= IRF_IMM;
            i->imm = opt_fit(t, y);
        }
        if (!cb) {
            if (i->a == i->b) {
                if (op == IR_SUB || op == IR_XOR) {
                    opt_make_const(i, 0);
                    return -1;
                }
                if (op == IR_AND || op == IR_OR) {
                    return i->a;
                }
            }
            return -1;
        }
        y = opt_fit(t, y);
        if (op == IR_SUB) {
            i->op = IR_ADD;
            op = IR_ADD;
            if (y == (t == IR_I32 ? (long)INT_MIN : LONG_MIN)) {
                i->flags &= ~IRF_NSW;   /* x - INT_MIN is not x + INT_MIN as far as overflow goes */
            }
            y = opt_fit(t, (long)(0 - (unsigned long)y));
            i->imm = y;
        }
        uy = opt_unsigned(t, y);
        switch (op) {
            case IR_ADD:
            case IR_OR:
            case IR_XOR:
                if (y == 0) {
                    return i->a;
                }
                break;
            case IR_SHL:
            case IR_LSHR:
            case IR_ASHR:
                if ((y & (bits - 1)) == 0) {
                    return i->a;
                }
                break;
            case IR_MUL:
            case IR_SDIV:
            case IR_UDIV:
                if (y == 1) {
                    return i->a;
                }
                break;
            default:
                break;
        }
        if ((op == IR_MUL || op == IR_AND) && y == 0) {
            opt_make_const(i, 0);
            return -1;
        }
        if ((op == IR_UREM && y == 1) || (op == IR_SREM && (y == 1 || y == -1))) {
            /* (an unsigned -1 is the largest value, by which the remainder is not 0) */
            opt_make_const(i, 0);
            return -1;
        }
        if (op == IR_AND && (uy & all) == all) {
            return i->a;
        }
        if (op == IR_OR && (uy & all) == all) {
            opt_make_const(i, -1);
            return -1;
        }
        if (op == IR_MUL && y > 0 && (k = opt_log2(uy)) > 0) {
            i->op = IR_SHL;
            op = IR_SHL;
            i->imm = y = k;
        } else if (op == IR_UDIV && (k = opt_log2(uy)) > 0) {
            i->op = IR_LSHR;
            op = IR_LSHR;
            i->imm = y = k;
        } else if (op == IR_UREM && (k = opt_log2(uy)) > 0) {
            i->op = IR_AND;
            op = IR_AND;
            i->imm = y = opt_fit(t, (long)(uy - 1));
            uy = opt_unsigned(t, y);
        }
        /* (x op c1) op c2 */
        if (opt_def[i->a] >= 0) {
            const IRInst *in = &opt_I[opt_def[i->a]];
            /* only when nothing else reads the inner result, which would keep x alive alongside it (the counts
               are right once copies have been propagated, which the first round does) */
            if (in->op == op && (in->flags & IRF_IMM) && in->type == t && in->a >= 0 && opt_is_var(in->a) && opt_nuse[i->a] == 1
                    && opt_round > 0) {
                long c1 = in->imm;
                int fold = 1;
                switch (op) {
                    case IR_ADD: y = (long)((unsigned long)c1 + (unsigned long)y); break;
                    case IR_AND: y = c1 & y; break;
                    case IR_OR: y = c1 | y; break;
                    case IR_XOR: y = c1 ^ y; break;
                    case IR_SHL:
                    case IR_LSHR:
                        if ((c1 & (bits - 1)) + (y & (bits - 1)) >= bits) {
                            fold = 0;
                        } else {
                            y = (c1 & (bits - 1)) + (y & (bits - 1));
                        }
                        break;
                    default: fold = 0; break;
                }
                if (fold) {
                    i->a = in->a;
                    i->imm = opt_fit(t, y);
                    i->flags &= ~IRF_NSW;   /* the sum of the constants may overflow where neither step did */
                    if (op != IR_AND && i->imm == 0) {
                        return i->a;
                    }
                    uy = opt_unsigned(t, i->imm);
                }
            }
        }
        if (op == IR_AND && (opt_maybe_set(i->a, 0) & ~uy & all) == 0) {
            return i->a;
        }
        return -1;
    }
    if (opt_fold_float(i)) {
        return -1;
    }
    if (op == IR_FMUL) {
        /* x * 2 is x + x, exactly */
        double c;
        if (opt_fconst(i->b, &c) && c == 2.0) {
            i->op = IR_FADD;
            i->b = i->a;
        } else if (opt_fconst(i->a, &c) && c == 2.0) {
            i->op = IR_FADD;
            i->a = i->b;
        }
        return -1;
    }
    switch (op) {
        case IR_NEG:
        case IR_NOT:
            if (t != IR_I32 && t != IR_I64) {
                return -1;
            }
            if (opt_const(i->a, &x)) {
                opt_make_const(i, op == IR_NEG ? (long)(0 - (unsigned long)x) : ~x);
                return -1;
            }
            if (opt_def[i->a] >= 0 && opt_I[opt_def[i->a]].op == op && opt_I[opt_def[i->a]].type == t) {
                return opt_I[opt_def[i->a]].a;
            }
            return -1;

        case IR_ICMP: {
            IRCond cc = (IRCond)i->cc;
            if (t != IR_I32 && t != IR_I64) {
                return -1;
            }
            ca = opt_const(i->a, &x);
            if (i->flags & IRF_IMM) {
                cb = 1;
                y = i->imm;
            } else {
                cb = opt_const(i->b, &y);
            }
            if (ca && cb) {
                opt_make_const(i, opt_fold_cmp(cc, t, x, y));
                return -1;
            }
            if (ca) {
                i->a = i->b;
                i->b = -1;
                i->flags |= IRF_IMM;
                i->imm = opt_fit(t, x);
                i->cc = (unsigned char)opt_swap_cond(cc);
                cc = (IRCond)i->cc;
                cb = 1;
                y = x;
            } else if (cb && !(i->flags & IRF_IMM)) {
                i->b = -1;
                i->flags |= IRF_IMM;
                i->imm = opt_fit(t, y);
            }
            if (!cb) {
                if (i->a == i->b) {
                    opt_make_const(i, cc == IR_EQ || cc == IR_GE || cc == IR_LE || cc == IR_UGE || cc == IR_ULE);
                }
                return -1;
            }
            if (i->imm == 0) {
                const IRInst *in = opt_def[i->a] >= 0 ? &opt_I[opt_def[i->a]] : NULL;
                if (cc == IR_ULT) {
                    opt_make_const(i, 0);
                    return -1;
                }
                if (cc == IR_UGE) {
                    opt_make_const(i, 1);
                    return -1;
                }
                if (cc == IR_UGT) {
                    i->cc = IR_NE;
                    cc = IR_NE;
                } else if (cc == IR_ULE) {
                    i->cc = IR_EQ;
                    cc = IR_EQ;
                }
                if (in && in->op == IR_ICMP && t == IR_I32 && (cc == IR_NE || cc == IR_EQ)
                        && (in->a < 0 || opt_is_var(in->a)) && (in->b < 0 || opt_is_var(in->b))) {
                    if (cc == IR_NE) {
                        return i->a;
                    }
                    /* !(a cc b) */
                    i->cc = (unsigned char)opt_invert_cond((IRCond)in->cc);
                    i->type = in->type;
                    i->flags = in->flags;
                    i->a = in->a;
                    i->b = in->b;
                    i->imm = in->imm;
                }
            }
            return -1;
        }

        case IR_SEXT:
        case IR_ZEXT: {
            const IRInst *in;
            if (!opt_is_int(i->a)) {
                return -1;
            }
            if (opt_const(i->a, &x)) {
                switch (i->size) {
                    case 1: x = op == IR_SEXT ? (long)(signed char)x : (long)(unsigned char)x; break;
                    case 2: x = op == IR_SEXT ? (long)(short)x : (long)(unsigned short)x; break;
                    case 4: x = op == IR_SEXT ? (long)(int)x : (long)(unsigned)x; break;
                    default: break;
                }
                opt_make_const(i, x);
                return -1;
            }
            if (opt_vtype(i->a) != t) {
                return -1;
            }
            if (op == IR_ZEXT && i->size < 8 && (opt_maybe_set(i->a, 0) & ~((1UL << (8 * i->size)) - 1)) == 0) {
                return i->a;
            }
            in = opt_def[i->a] >= 0 ? &opt_I[opt_def[i->a]] : NULL;
            if (in && in->op == op && in->size <= i->size && in->type == t) {
                return i->a;
            }
            if (in && op == IR_SEXT && in->op == IR_LOAD && (in->flags & IRF_SIGNED) && in->size <= i->size) {
                return i->a;
            }
            return -1;
        }

        case IR_TRUNC: {
            const IRInst *in;
            if (!opt_is_int(i->a) || t != IR_I32) {
                return -1;
            }
            if (opt_const(i->a, &x)) {
                opt_make_const(i, x);
                return -1;
            }
            in = opt_def[i->a] >= 0 ? &opt_I[opt_def[i->a]] : NULL;
            if (in && (in->op == IR_SEXT || in->op == IR_ZEXT) && in->a >= 0 && opt_vtype(in->a) == IR_I32) {
                if (in->size >= 4) {
                    return in->a;
                }
                i->op = in->op;
                i->size = in->size;
                i->a = in->a;
            }
            return -1;
        }

        default:
            return -1;
    }
}

/* ------------------------------------------------------------------ */
/* value numbering                                                    */
/* ------------------------------------------------------------------ */

static int *opt_vn = NULL;      /* open addressing: instruction indices, -1 empty */
static int opt_vnmask = 0;

/* an instruction whose result depends only on its operands */
static int opt_pure(const IRInst *i) {
    switch (i->op) {
        case IR_ICONST: case IR_FCONST: case IR_ADDR_LOCAL: case IR_ADDR_GLOBAL: case IR_ADDR_STRING:
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_SDIV: case IR_UDIV: case IR_SREM: case IR_UREM:
        case IR_SHL: case IR_LSHR: case IR_ASHR: case IR_AND: case IR_OR: case IR_XOR: case IR_NEG: case IR_NOT:
        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV: case IR_FNEG: case IR_FSQRT: case IR_FABS: case IR_FRINT:
        case IR_ICMP: case IR_FCMP: case IR_SEXT: case IR_ZEXT: case IR_TRUNC:
        case IR_ITOF: case IR_FTOI: case IR_FTOF: case IR_BITCAST: case IR_BSWAP: case IR_CLZ: case IR_CTZ:
            return 1;
        default:
            return 0;
    }
}

static unsigned long opt_hash(const IRInst *i) {
    unsigned long h = (unsigned long)i->op * 31u + i->type;
    h = h * 1000003u ^ (unsigned long)i->t2;
    h = h * 1000003u ^ (unsigned long)i->size;
    h = h * 1000003u ^ (unsigned long)i->flags;
    h = h * 1000003u ^ (unsigned long)i->cc;
    h = h * 1000003u ^ (unsigned long)(long)i->a;
    h = h * 1000003u ^ (unsigned long)(long)i->b;
    h = h * 1000003u ^ (unsigned long)i->imm;
    h = h * 1000003u ^ (unsigned long)(long)i->aux;
    if (i->sym) {
        const char *s;
        for (s = i->sym; *s; s++) {
            h = h * 31u + (unsigned char)*s;
        }
    }
    return h ^ (h >> 17);
}

static int opt_same(const IRInst *x, const IRInst *y) {
    if (x->op != y->op || x->type != y->type || x->t2 != y->t2 || x->size != y->size || x->flags != y->flags
            || x->cc != y->cc || x->a != y->a || x->b != y->b || x->imm != y->imm || x->aux != y->aux) {
        return 0;
    }
    if (x->sym || y->sym) {
        return x->sym && y->sym && strcmp(x->sym, y->sym) == 0;
    }
    return 1;
}

/* an equal instruction in a block dominating `b` (or earlier in b), or -1; otherwise ii is entered */
static int opt_vn_find(int ii, int b) {
    unsigned long h = opt_hash(&opt_I[ii]);
    int s = (int)(h & (unsigned long)opt_vnmask);
    while (opt_vn[s] >= 0) {
        int j = opt_vn[s];
        if (opt_I[j].op != IR_NOP && opt_same(&opt_I[j], &opt_I[ii]) && opt_dominates(opt_blk[j], b)) {
            return j;
        }
        s = (s + 1) & opt_vnmask;
    }
    opt_vn[s] = ii;
    return -1;
}

/* operands in a canonical order, so that a + b and b + a are numbered alike */
static void opt_canonical(IRInst *i) {
    if (i->a >= 0 && i->b >= 0 && i->a > i->b) {
        int t;
        if (opt_commutative((IROp)i->op)) {
            t = i->a;
            i->a = i->b;
            i->b = t;
        } else if (i->op == IR_ICMP) {
            t = i->a;
            i->a = i->b;
            i->b = t;
            i->cc = (unsigned char)opt_swap_cond((IRCond)i->cc);
        }
    }
}

/* ------------------------------------------------------------------ */
/* memory: what is known to be at an address                          */
/* ------------------------------------------------------------------ */

#define OPT_MEM_FRAME 0     /* a frame slot */
#define OPT_MEM_SYM 1       /* a global */
#define OPT_MEM_REG 2       /* a pointer in a register */
#define OPT_MAXMEM 24

typedef struct OptMem {
    int kind;
    int base;               /* OPT_MEM_REG: the register */
    const char *sym;        /* OPT_MEM_SYM */
    long off;
    int size;
    int type;
    int flags;              /* a load's IRF_SIGNED */
    int value;
    int stored;             /* known from a store rather than a load */
} OptMem;

static OptMem opt_mem[OPT_MAXMEM];
static int opt_nmem = 0;
static OptMem *opt_memsave = NULL;  /* per block, the state at its end */
static int *opt_nmemsave = NULL;
static int opt_frame_escapes = 0;   /* a frame slot's address reaches something other than a load or store */

/* the base and constant offset of the address in register v */
static void opt_addr(int v, OptMem *m) {
    int depth;
    m->kind = OPT_MEM_REG;
    m->base = v;
    m->sym = NULL;
    m->off = 0;
    for (depth = 0; depth < 8; depth++) {
        const IRInst *d;
        if (opt_def[v] < 0) {
            break;
        }
        d = &opt_I[opt_def[v]];
        if (d->op == IR_ADD && (d->flags & IRF_IMM) && d->type == IR_I64 && d->a >= 0 && opt_is_var(d->a)) {
            m->off += d->imm;
            v = d->a;
            m->base = v;
            continue;
        }
        if (d->op == IR_ADDR_LOCAL) {
            m->kind = OPT_MEM_FRAME;
            m->base = -1;
            m->off += d->imm;
        } else if (d->op == IR_ADDR_GLOBAL) {
            m->kind = OPT_MEM_SYM;
            m->base = -1;
            m->sym = d->sym;
        }
        break;
    }
}

static int opt_same_place(const OptMem *a, const OptMem *b) {
    if (a->kind != b->kind) {
        return 0;
    }
    if (a->kind == OPT_MEM_REG) {
        return a->base == b->base;
    }
    if (a->kind == OPT_MEM_SYM) {
        return strcmp(a->sym, b->sym) == 0;
    }
    return 1;
}

static int opt_may_alias(const OptMem *a, long asize, const OptMem *b, long bsize) {
    if (opt_same_place(a, b)) {
        return a->off < b->off + bsize && b->off < a->off + asize;
    }
    if (a->kind == OPT_MEM_REG || b->kind == OPT_MEM_REG) {
        /* a pointer may point anywhere but into frame slots whose address never escaped */
        const OptMem *o = a->kind == OPT_MEM_REG ? b : a;
        return o->kind != OPT_MEM_FRAME || opt_frame_escapes;
    }
    return 0;      /* two globals, or a global and a frame slot */
}

static void opt_mem_kill(const OptMem *m, long size) {
    int k, n = 0;
    for (k = 0; k < opt_nmem; k++) {
        if (!opt_may_alias(&opt_mem[k], opt_mem[k].size, m, size)) {
            opt_mem[n++] = opt_mem[k];
        }
    }
    opt_nmem = n;
}

/* a call: anything but frame slots that never escaped */
static void opt_mem_kill_call(void) {
    int k, n = 0;
    for (k = 0; k < opt_nmem; k++) {
        if (opt_mem[k].kind == OPT_MEM_FRAME && !opt_frame_escapes) {
            opt_mem[n++] = opt_mem[k];
        }
    }
    opt_nmem = n;
}

static void opt_mem_add(const OptMem *m) {
    if (opt_nmem == OPT_MAXMEM) {
        memmove(&opt_mem[0], &opt_mem[1], (OPT_MAXMEM - 1) * sizeof(OptMem));
        opt_nmem--;
    }
    opt_mem[opt_nmem++] = *m;
}

/* whether any frame slot's address is used other than as the address of a load, store, copy or zero */
static void opt_find_escapes(void) {
    int k, j, m, n;
    opt_frame_escapes = 0;
    for (k = 0; k < opt_F->nvregs; k++) {
        opt_vflag[k] = 0;
    }
    /* a register escapes when it is read other than as an address */
    for (k = 0; k < opt_nrpo; k++) {
        int b = opt_rpo[k];
        for (j = 0; j < opt_B[b].n; j++) {
            IRInst *i = &opt_I[opt_B[b].code[j]];
            n = opt_nops(i);
            for (m = 0; m < n; m++) {
                int u = *opt_op(i, m);
                int as_addr;
                if (u < 0) {
                    continue;
                }
                as_addr = m == 0 && (i->op == IR_LOAD || i->op == IR_STORE || i->op == IR_LOADBITS || i->op == IR_STOREBITS
                        || i->op == IR_ZERO || i->op == IR_COPY);
                as_addr |= m == 1 && i->op == IR_COPY;
                as_addr |= m == 0 && i->op == IR_ADD && (i->flags & IRF_IMM) && i->type == IR_I64;
                if (!as_addr) {
                    opt_vflag[u] = 1;
                }
            }
        }
    }
    /* through the adds, from the last to the first, which define their operands' users before them in reverse postorder */
    for (k = opt_nrpo - 1; k >= 0; k--) {
        int b = opt_rpo[k];
        for (j = opt_B[b].n - 1; j >= 0; j--) {
            IRInst *i = &opt_I[opt_B[b].code[j]];
            if (i->op == IR_ADD && (i->flags & IRF_IMM) && i->type == IR_I64 && i->dst >= 0 && opt_vflag[i->dst] && i->a >= 0) {
                opt_vflag[i->a] = 1;
            }
        }
    }
    for (k = 0; k < opt_nrpo; k++) {
        int b = opt_rpo[k];
        for (j = 0; j < opt_B[b].n; j++) {
            IRInst *i = &opt_I[opt_B[b].code[j]];
            if (i->op == IR_ADDR_LOCAL && i->dst >= 0 && opt_vflag[i->dst]) {
                opt_frame_escapes = 1;
            }
        }
    }
    /* a loop can carry an add's result around to its own operand, which the pass above sees only once */
    for (k = 0; k < opt_F->nvregs && !opt_frame_escapes; k++) {
        if (opt_vflag[k] && opt_def[k] >= 0 && opt_I[opt_def[k]].op == OPT_PHI) {
            opt_frame_escapes = 1;
        }
    }
}

/* the memory effects of instruction i on what is known; returns a register a load can be replaced with, or -1 */
static int opt_mem_inst(IRInst *i) {
    OptMem m;
    int k;
    switch (i->op) {
        case IR_LOAD:
            if ((i->flags & IRF_VOLATILE) || i->dst < 0 || !opt_is_var(i->dst) || !opt_is_var(i->a)) {
                return -1;
            }
            opt_addr(i->a, &m);
            for (k = opt_nmem - 1; k >= 0; k--) {
                const OptMem *e = &opt_mem[k];
                if (!opt_same_place(e, &m) || e->off != m.off || e->size != i->size || e->type != i->type) {
                    continue;
                }
                if (!e->stored) {
                    if ((e->flags & IRF_SIGNED) == (i->flags & IRF_SIGNED)) {
                        return e->value;
                    }
                } else if ((size_t)e->size == ir_type_size((IRType)i->type)) {
                    return e->value;
                } else if (ir_type_is_float((IRType)i->type) == 0) {
                    /* a narrow store: the loaded value is the stored one extended */
                    i->op = (unsigned char)((i->flags & IRF_SIGNED) ? IR_SEXT : IR_ZEXT);
                    i->a = e->value;
                    i->flags = 0;
                    return -1;
                }
                break;
            }
            m.size = i->size;
            m.type = i->type;
            m.flags = i->flags & IRF_SIGNED;
            m.value = i->dst;
            m.stored = 0;
            opt_mem_add(&m);
            return -1;

        case IR_STORE:
            opt_addr(i->a, &m);
            opt_mem_kill(&m, i->size);
            if (!(i->flags & IRF_VOLATILE) && opt_is_var(i->b) && opt_vtype(i->b) == i->type) {
                m.size = i->size;
                m.type = i->type;
                m.flags = 0;
                m.value = i->b;
                m.stored = 1;
                opt_mem_add(&m);
            }
            return -1;

        case IR_STOREBITS:
            opt_addr(i->a, &m);
            opt_mem_kill(&m, i->size);
            return -1;

        case IR_COPY:
        case IR_ZERO:
            opt_addr(i->a, &m);
            opt_mem_kill(&m, i->imm);
            return -1;

        case IR_CALL:
            opt_mem_kill_call();
            return -1;

        case IR_VASTART:
        case IR_VAARG:
        case IR_FENCE:
            opt_nmem = 0;
            return -1;

        default:
            return -1;
    }
}

/* ------------------------------------------------------------------ */
/* the simplification walk                                            */
/* ------------------------------------------------------------------ */

static int opt_cfg_changed;

/* a branch on a constant becomes a jump; a branch on a compare with zero branches on the value itself */
static int opt_branch(int b, IRInst *t) {
    long x;
    int c = t->a;
    if (opt_const(c, &x)) {
        int taken = x ? (int)t->imm : t->aux;
        int other = x ? t->aux : (int)t->imm;
        t->op = IR_BR;
        t->a = -1;
        t->type = IR_VOID;
        t->imm = taken;
        opt_remove_pred(other, b);
        opt_cfg_changed = 1;
        return 1;
    }
    if (opt_def[c] >= 0) {
        const IRInst *d = &opt_I[opt_def[c]];
        if (d->op == IR_ICMP && (d->flags & IRF_IMM) && d->imm == 0 && (d->cc == IR_NE || d->cc == IR_EQ)
                && (d->type == IR_I32 || d->type == IR_I64) && opt_is_var(d->a)) {
            t->a = d->a;
            t->type = d->type;
            if (d->cc == IR_EQ) {
                int s = (int)t->imm;
                t->imm = t->aux;
                t->aux = s;
            }
            return 1;
        }
    }
    return 0;
}

/* a switch on a constant becomes a jump */
static int opt_switch(int b, IRInst *t) {
    long x;
    int n = ir_nsuccs(opt_F, t);
    int k, taken_k, taken;
    if (!opt_const(t->a, &x)) {
        return 0;
    }
    taken_k = (unsigned long)x < (unsigned long)(n - 1) ? (int)x + 1 : 0;
    taken = ir_succ(opt_F, t, taken_k);
    for (k = 0; k < n; k++) {
        if (k != taken_k) {
            opt_remove_pred(ir_succ(opt_F, t, k), b);
        }
    }
    t->op = IR_BR;
    t->a = -1;
    t->type = IR_VOID;
    t->imm = taken;
    t->aux = -1;
    opt_cfg_changed = 1;
    return 1;
}

static int opt_walk(void) {
    int changed = 0;
    int k, j, n, ninst = 0;
    opt_index();
    for (k = 0; k < opt_nrpo; k++) {
        ninst += opt_B[opt_rpo[k]].n;
    }
    n = 64;
    while (n < 2 * ninst) {
        n *= 2;
    }
    opt_vn = xrealloc(opt_vn, (size_t)n * sizeof(int));
    opt_vnmask = n - 1;
    for (k = 0; k < n; k++) {
        opt_vn[k] = -1;
    }
    for (k = 0; k < opt_F->nvregs; k++) {
        opt_repl[k] = -1;
    }
    opt_find_escapes();
    opt_memsave = xrealloc(opt_memsave, (size_t)(opt_nb + 1) * OPT_MAXMEM * sizeof(OptMem));
    opt_nmemsave = xrealloc(opt_nmemsave, (size_t)(opt_nb + 1) * sizeof(int));
    opt_cfg_changed = 0;

    for (k = 0; k < opt_nrpo; k++) {
        int b = opt_rpo[k];
        OptBlock *ob = &opt_B[b];
        /* what is known about memory carries over from the only predecessor */
        opt_nmem = 0;
        if (ob->npreds == 1 && opt_B[ob->preds[0]].rpo < k) {
            int p = ob->preds[0];
            opt_nmem = opt_nmemsave[p];
            memcpy(opt_mem, &opt_memsave[(size_t)p * OPT_MAXMEM], (size_t)opt_nmem * sizeof(OptMem));
        }
        for (j = 0; j < ob->n; j++) {
            int ii = ob->code[j];
            IRInst *i = &opt_I[ii];
            int m, nops, r;
            if (i->op == IR_NOP) {
                continue;
            }
            nops = opt_nops(i);
            for (m = 0; m < nops; m++) {
                int *p = opt_op(i, m);
                if (*p >= 0) {
                    *p = opt_resolve(*p);
                }
            }
            if (i->op == OPT_PHI) {
                const OptPhi *ph = &opt_P[i->imm];
                int same = -1, undef = -1;
                for (m = 0; m < ph->n; m++) {
                    int v = opt_resolve(ph->v[m]);
                    if (v == i->dst || v == same) {
                        continue;
                    }
                    if (opt_is_undef(v)) {
                        undef = v;
                        continue;
                    }
                    if (same >= 0) {
                        same = -2;
                        break;
                    }
                    same = v;
                }
                /* undefined on some paths and v on the others: v, when it is available on all of them, that is when
                   it is defined before the block (a phi of this block, on a back edge, is the last iteration's) */
                if (same >= 0 && undef >= 0 && opt_def[same] >= 0
                        && (opt_blk[opt_def[same]] == b || !opt_dominates(opt_blk[opt_def[same]], b))) {
                    same = -2;
                }
                if (same == -1 && undef >= 0) {
                    same = undef;
                }
                if (same >= 0) {
                    opt_repl[i->dst] = same;
                    opt_kill(i);
                    changed = 1;
                }
                continue;
            }
            if (i->op == IR_CONDBR) {
                changed |= opt_branch(b, i);
                continue;
            }
            if (i->op == IR_SWITCH) {
                changed |= opt_switch(b, i);
                continue;
            }
            r = opt_simplify(i);
            if (r >= 0 && r != i->dst) {
                opt_repl[i->dst] = r;
                opt_kill(i);
                changed = 1;
                continue;
            }
            /* a store writes the low bytes of its value: a narrowing or an extension first changes none of them */
            if (i->op == IR_STORE && !(i->flags & IRF_VOLATILE) && i->b >= 0 && opt_def[i->b] >= 0) {
                const IRInst *d = &opt_I[opt_def[i->b]];
                if (d->a >= 0 && opt_is_int(d->a) && opt_is_int(i->b)
                        && ((d->op == IR_TRUNC && i->size <= 4) || ((d->op == IR_SEXT || d->op == IR_ZEXT) && i->size <= d->size))) {
                    i->b = d->a;
                    i->type = (unsigned char)opt_vtype(d->a);
                    changed = 1;
                }
            }
            if (i->op == IR_LOAD || i->op == IR_STORE || i->op == IR_STOREBITS || i->op == IR_COPY || i->op == IR_ZERO
                    || i->op == IR_CALL || i->op == IR_VASTART || i->op == IR_VAARG || i->op == IR_FENCE) {
                r = opt_mem_inst(i);
                if (r >= 0) {
                    opt_repl[i->dst] = r;
                    opt_kill(i);
                    changed = 1;
                    continue;
                }
                if (i->op == IR_LOAD) {
                    continue;
                }
            }
            /* a constant offset from a pointer folds into each memory access using it, so it is not worth sharing */
            if (opt_pure(i) && opt_all_vars(i) && !(i->op == IR_ADD && (i->flags & IRF_IMM) && i->type == IR_I64)) {
                int e;
                opt_canonical(i);
                opt_def[i->dst] = ii;
                e = opt_vn_find(ii, b);
                if (e >= 0) {
                    opt_repl[i->dst] = opt_I[e].dst;
                    opt_kill(i);
                    changed = 1;
                    continue;
                }
            }
            if (i->dst >= 0) {
                opt_def[i->dst] = ii;
            }
        }
        opt_nmemsave[b] = opt_nmem;
        memcpy(&opt_memsave[(size_t)b * OPT_MAXMEM], opt_mem, (size_t)opt_nmem * sizeof(OptMem));
    }
    opt_apply_repl();
    opt_compact();
    return changed || opt_cfg_changed;
}

/* ------------------------------------------------------------------ */
/* jump threading                                                     */
/* ------------------------------------------------------------------ */

/* the phi of block b defining v, or NULL */
static IRInst *opt_phi_of(int b, int v) {
    int d = v >= 0 ? opt_def[v] : -1;
    if (d < 0 || opt_I[d].op != OPT_PHI || opt_blk[d] != b) {
        return NULL;
    }
    return &opt_I[d];
}

/* the uses of v that are phi arguments in b's successors, for edges from b */
static int opt_succ_phi_uses(int b, int v) {
    int n = opt_nsucc(b), k, j, e, count = 0;
    for (k = 0; k < n; k++) {
        OptBlock *sb = &opt_B[opt_succ(b, k)];
        if (!opt_first_edge(b, k)) {
            continue;
        }
        for (e = 0; e < sb->npreds; e++) {
            if (sb->preds[e] != b) {
                continue;
            }
            for (j = 0; j < sb->n && opt_I[sb->code[j]].op == OPT_PHI; j++) {
                count += opt_P[opt_I[sb->code[j]].imm].v[e] == v;
            }
        }
    }
    return count;
}

/*
 * Predecessor k of block b jumps to `target` instead, a successor of b:
 * the target's phis take, for the new edge, what came to them through b
 * (b's own phi arguments for that predecessor where they are b's phis).
 */
static void opt_redirect(int b, int k, int target) {
    int pred = opt_B[b].preds[k];
    OptBlock *tb = &opt_B[target];
    int e, n = opt_nsucc(pred), kb, j;
    for (e = 0; e < n && opt_succ(pred, e) != b; e++) {
    }
    ir_set_succ(opt_F, opt_term(pred), e, target);
    for (kb = 0; kb < tb->npreds && tb->preds[kb] != b; kb++) {
    }
    opt_add_pred(target, pred);
    tb = &opt_B[target];
    for (j = 0; j < tb->n && opt_I[tb->code[j]].op == OPT_PHI; j++) {
        OptPhi *ph = &opt_P[opt_I[tb->code[j]].imm];
        int v = ph->v[kb];
        IRInst *bp = opt_phi_of(b, v);
        if (bp) {
            v = opt_P[bp->imm].v[k];
        }
        ph->v = xrealloc(ph->v, (size_t)(ph->n + 2) * sizeof(int));
        ph->v[ph->n++] = v;
        if (v >= 0) {
            opt_nuse[v]++;
        }
    }
    /* b loses the edge and its phis their arguments for it */
    for (j = 0; j < opt_B[b].n && opt_I[opt_B[b].code[j]].op == OPT_PHI; j++) {
        int v = opt_P[opt_I[opt_B[b].code[j]].imm].v[k];
        if (v >= 0) {
            opt_nuse[v]--;
        }
    }
    opt_remove_pred(b, pred);
}

/*
 * A block that is only phis and a branch on one of them, whose argument
 * from some predecessor is a constant: that predecessor can go straight
 * to the branch's target. C's `if (a ? (x = .., 1) : 0)` and the
 * conditional expressions of macros make these. The phis must be read
 * only by the branch and by the successors' phis, which take the value
 * that came through this block from the threaded predecessor directly.
 * opt_nuse is kept up to date, so that blocks can be threaded one after
 * another without indexing the function again.
 */
static int opt_thread_block(int b) {
    OptBlock *ob = &opt_B[b];
    IRInst *t, *cond;
    int j, k, changed = 0;
    if (!ob->live || ob->n < 2 || ob->n > 64) {
        return 0;
    }
    t = opt_term(b);
    if (t->op != IR_CONDBR || !(cond = opt_phi_of(b, t->a)) || t->imm == t->aux) {
        return 0;
    }
    for (j = 0; j < ob->n - 1; j++) {
        IRInst *p = &opt_I[ob->code[j]];
        if (p->op != OPT_PHI || opt_nuse[p->dst] != (p == cond) + opt_succ_phi_uses(b, p->dst)) {
            return 0;
        }
    }
    for (k = 0; k < opt_B[b].npreds; k++) {
        long x;
        int pred = opt_B[b].preds[k];
        int target, e, n;
        if (!opt_const(opt_P[cond->imm].v[k], &x)) {
            continue;
        }
        target = x ? (int)t->imm : t->aux;
        n = opt_nsucc(pred);
        for (e = 0; e < n && opt_succ(pred, e) != target; e++) {
        }
        if (target == b || e < n) {
            continue;   /* a loop on itself, or the predecessor already goes there */
        }
        opt_redirect(b, k, target);
        k--;
        changed = 1;
    }
    return changed;
}

/*
 * A block that is only phis and a jump: its predecessors can jump past
 * it, each taking its phis' arguments straight into the successor's
 * phis, when the phis are read by nothing else and no predecessor
 * already jumps to the successor.
 */
static int opt_skip_block(int b) {
    OptBlock *ob = &opt_B[b];
    IRInst *t;
    int j, k, s, n, any = 0;
    if (!ob->live || ob->n > 64) {
        return 0;
    }
    t = opt_term(b);
    if (t->op != IR_BR || (s = (int)t->imm) == b) {
        return 0;
    }
    for (j = 0; j < ob->n - 1; j++) {
        IRInst *p = &opt_I[ob->code[j]];
        if (p->op != OPT_PHI || opt_nuse[p->dst] != opt_succ_phi_uses(b, p->dst)) {
            return 0;
        }
    }
    for (k = 0; k < opt_B[b].npreds; k++) {
        int pred = opt_B[b].preds[k];
        int e;
        n = opt_nsucc(pred);
        for (e = 0; e < n && opt_succ(pred, e) != s; e++) {
        }
        if (e < n || pred == b) {
            continue;
        }
        opt_redirect(b, k, s);
        k--;
        any = 1;
    }
    return any;
}

/* a block whose only predecessor jumps to it joins that predecessor (its phis, with one argument, are gone by now) */
static int opt_merge_block(int b) {
    int s, j, q, r, nt;
    IRInst *t;
    if (!opt_B[b].live || opt_B[b].n == 0) {
        return 0;
    }
    t = opt_term(b);
    if (t->op != IR_BR || (s = (int)t->imm) == b || s == 0 || opt_B[s].npreds != 1 || !opt_B[s].live) {
        return 0;
    }
    for (j = 0; j < opt_B[s].n; j++) {
        if (opt_I[opt_B[s].code[j]].op == OPT_PHI) {
            return 0;
        }
    }
    opt_B[b].n--;
    for (j = 0; j < opt_B[s].n; j++) {
        opt_push(b, opt_B[s].code[j]);
    }
    nt = opt_nsucc(b);
    for (q = 0; q < nt; q++) {
        int t2 = opt_succ(b, q);
        if (!opt_first_edge(b, q)) {
            continue;
        }
        for (r = 0; r < opt_B[t2].npreds; r++) {
            if (opt_B[t2].preds[r] == s) {
                opt_B[t2].preds[r] = b;
            }
        }
    }
    opt_B[s].n = 0;
    opt_B[s].live = 0;
    opt_B[s].npreds = 0;
    return 1;
}

static int opt_thread(void) {
    int b, changed = 0;
    if (opt_cfg_changed) {
        return 0;   /* the dominators are out of date until the next round */
    }
    for (b = 0; b < opt_nb; b++) {
        while (opt_merge_block(b)) {
            changed = 1;
        }
    }
    opt_index();
    for (b = 1; b < opt_nb; b++) {
        changed |= opt_skip_block(b) || opt_thread_block(b);
    }
    if (changed) {
        opt_cfg_changed = 1;
    }
    return changed;
}

/* ------------------------------------------------------------------ */
/* dead code                                                          */
/* ------------------------------------------------------------------ */

static int opt_removable(const IRInst *i) {
    switch (i->op) {
        case IR_STORE: case IR_STOREBITS: case IR_COPY: case IR_ZERO: case IR_CALL: case IR_VASTART: case IR_VAARG:
        case IR_FENCE: case IR_ALLOCA: case IR_BR: case IR_CONDBR: case IR_RET: case IR_UNREACHABLE: case IR_SWITCH:
            return 0;
        case IR_LOAD:
        case IR_LOADBITS:
            return !(i->flags & IRF_VOLATILE);
        default:
            return 1;
    }
}

/* mark from the instructions that must stay; registers with several definitions keep all of them */
static int opt_dce(void) {
    int *dhead, *dnext, *work;
    int nw = 0, k, j, m, n, v, removed = 0;
    opt_iroom();
    opt_vroom();
    dhead = xmalloc((size_t)(opt_F->nvregs + 1) * sizeof(int));
    dnext = xmalloc((size_t)(opt_ni + 1) * sizeof(int));
    work = xmalloc((size_t)(opt_ni + 1) * sizeof(int));
    for (v = 0; v < opt_F->nvregs; v++) {
        dhead[v] = -1;
        opt_vflag[v] = 0;
    }
    for (k = 0; k < opt_nrpo; k++) {
        int b = opt_rpo[k];
        for (j = 0; j < opt_B[b].n; j++) {
            int ii = opt_B[b].code[j];
            IRInst *i = &opt_I[ii];
            opt_mark[ii] = 0;
            if (i->dst >= 0) {
                dnext[ii] = dhead[i->dst];
                dhead[i->dst] = ii;
            }
            if (!opt_removable(i)) {
                opt_mark[ii] = 1;
                work[nw++] = ii;
            }
        }
    }
    while (nw) {
        IRInst *i = &opt_I[work[--nw]];
        n = opt_nops(i);
        for (m = 0; m < n; m++) {
            int u = *opt_op(i, m);
            int d;
            if (u < 0 || opt_vflag[u]) {
                continue;
            }
            opt_vflag[u] = 1;
            for (d = dhead[u]; d >= 0; d = dnext[d]) {
                if (!opt_mark[d]) {
                    opt_mark[d] = 1;
                    work[nw++] = d;
                }
            }
        }
    }
    for (k = 0; k < opt_nrpo; k++) {
        int b = opt_rpo[k];
        for (j = 0; j < opt_B[b].n; j++) {
            int ii = opt_B[b].code[j];
            if (!opt_mark[ii]) {
                opt_kill(&opt_I[ii]);
                removed = 1;
            }
        }
    }
    free(dhead);
    free(dnext);
    free(work);
    opt_compact();
    return removed;
}

/* ------------------------------------------------------------------ */
/* sinking                                                            */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* loop-invariant code motion                                         */
/* ------------------------------------------------------------------ */

/* every read of v reads w instead */
static void opt_repl_all(int v, int w) {
    int b, j, m, n;
    for (b = 0; b < opt_nb; b++) {
        if (!opt_B[b].live) {
            continue;
        }
        for (j = 0; j < opt_B[b].n; j++) {
            IRInst *i = &opt_I[opt_B[b].code[j]];
            n = opt_nops(i);
            for (m = 0; m < n; m++) {
                int *p = opt_op(i, m);
                if (*p == v) {
                    *p = w;
                }
            }
        }
    }
}

static char *opt_hoisted = NULL;    /* per instruction: moved out of a loop, which rematerialization leaves alone */
static int opt_hcap = 0;

/* an invariant computation that may run where the loop's own code would not: no traps, no memory */
static int opt_hoistable(const IRInst *i) {
    if (!opt_pure(i) || !opt_all_vars(i)) {
        return 0;
    }
    /* an offset from a pointer, or a frame address, folds into the access using it: in a register it would cost one */
    if ((i->op == IR_ADD && (i->flags & IRF_IMM) && i->type == IR_I64) || i->op == IR_ADDR_LOCAL) {
        return 0;
    }
    switch (i->op) {
        case IR_SDIV: case IR_UDIV: case IR_SREM: case IR_UREM: case IR_FTOI:
            return 0;
        default:
            return 1;
    }
}

/* x is computed outside the loop (or is a parameter) */
static int opt_invariant(int x, const char *in) {
    return x >= 0 && (opt_def[x] < 0 || !in[opt_blk[opt_def[x]]]);
}

static int opt_new_inst(int b, int at, IROp op, IRType t, int a, int b2, long imm, int flags) {
    IRInst ins;
    memset(&ins, 0, sizeof(ins));
    ins.op = (unsigned char)op;
    ins.type = (unsigned char)t;
    ins.dst = opt_vreg(t, -1);
    ins.a = a;
    ins.b = b2;
    ins.aux = -1;
    ins.imm = imm;
    ins.flags = (unsigned char)flags;
    opt_insert(b, at, opt_inst(&ins));
    opt_iroom();
    opt_blk[opt_ni - 1] = b;
    opt_def[ins.dst] = opt_ni - 1;
    return ins.dst;
}

/*
 * Strength reduction: in a loop whose only latch is `latch`, an address
 * base + i*c, with i an induction variable (a phi of the header that
 * goes up by a constant step on the back edge; an i32 one through a
 * sign extension, and only when its add cannot overflow, C's signed
 * arithmetic) and base computed outside the loop, becomes a pointer of
 * its own: base + i0*c on entry, plus step*c on the back edge. Three
 * instructions an iteration (extend, multiply, add) become one.
 */
static int opt_reduce(int hd, int pre, const char *in) {
    int latch = -1, kpre = -1, klatch = -1, j, k, m, any = 0, nrep = 0;
    OptBlock *hb = &opt_B[hd];
    struct { int base; int iv; long c; int ptr; } rep[16];
    for (k = 0; k < hb->npreds; k++) {
        if (hb->preds[k] == pre) {
            kpre = k;
        } else if (latch < 0) {
            latch = hb->preds[k];
            klatch = k;
        } else {
            return 0;   /* several latches */
        }
    }
    if (latch < 0 || kpre < 0 || opt_nsucc(latch) < 1) {
        return 0;
    }
    for (k = 0; k < opt_nrpo; k++) {
        int b = opt_rpo[k];
        if (!in[b] || b == pre) {
            continue;
        }
        for (j = 0; j < opt_B[b].n; j++) {
            int ii = opt_B[b].code[j];
            IRInst *add = &opt_I[ii];
            int base = -1, prod = -1, x, iv, r;
            long c, step;
            IRInst *pi, *ph, *nx;
            if (add->op != IR_ADD || (add->flags & IRF_IMM) || add->type != IR_I64 || !opt_all_vars(add)) {
                continue;
            }
            for (m = 0; m < 2 && base < 0; m++) {
                int o = m == 0 ? add->a : add->b;
                int other = m == 0 ? add->b : add->a;
                if (opt_invariant(other, in) && opt_def[o] >= 0) {
                    base = other;
                    prod = o;
                }
            }
            if (base < 0) {
                continue;
            }
            /* prod = x * c, x << k, or x itself (c = 1); x = i (i64) or sext i (i32) */
            pi = &opt_I[opt_def[prod]];
            if ((pi->op == IR_MUL || pi->op == IR_SHL) && (pi->flags & IRF_IMM) && pi->type == IR_I64 && pi->a >= 0) {
                c = pi->op == IR_MUL ? pi->imm : (pi->imm >= 0 && pi->imm < 32 ? 1L << pi->imm : 0);
                x = pi->a;
            } else {
                c = 1;
                x = prod;
            }
            if (c == 0 || opt_def[x] < 0) {
                continue;
            }
            iv = x;
            if (opt_I[opt_def[x]].op == IR_SEXT && opt_I[opt_def[x]].size == 4 && opt_vtype(x) == IR_I64) {
                iv = opt_I[opt_def[x]].a;
            }
            if (iv < 0 || opt_def[iv] < 0 || (ph = &opt_I[opt_def[iv]])->op != OPT_PHI || opt_blk[opt_def[iv]] != hd) {
                continue;
            }
            /* the phi's back edge value is the phi plus a constant (signed int arithmetic for an i32 behind a sext) */
            {
                int nv = opt_P[ph->imm].v[klatch];
                if (nv < 0 || opt_def[nv] < 0) {
                    continue;
                }
                nx = &opt_I[opt_def[nv]];
                if (nx->op != IR_ADD || !(nx->flags & IRF_IMM) || nx->a != iv || nx->type != opt_vtype(iv)
                        || (opt_vtype(iv) == IR_I32 && (iv == x || !(nx->flags & IRF_NSW))) || (opt_vtype(iv) == IR_I64 && iv != x)) {
                    continue;
                }
                step = nx->imm;
            }
            for (r = 0; r < nrep; r++) {
                if (rep[r].base == base && rep[r].iv == iv && rep[r].c == c) {
                    break;
                }
            }
            if (r == nrep) {
                int init = opt_P[ph->imm].v[kpre];
                int e, t2, p0, ptr, pnext, pidx;
                IRInst phi;
                if (nrep == 16) {
                    continue;
                }
                /* the start, in the preheader: base + extend(init) * c */
                e = init;
                if (opt_vtype(iv) == IR_I32) {
                    e = opt_new_inst(pre, opt_B[pre].n - 1, IR_SEXT, IR_I64, init, -1, 0, 0);
                    opt_I[opt_def[e]].size = 4;
                }
                t2 = c == 1 ? e : opt_new_inst(pre, opt_B[pre].n - 1, IR_MUL, IR_I64, e, -1, c, IRF_IMM);
                p0 = opt_new_inst(pre, opt_B[pre].n - 1, IR_ADD, IR_I64, base, t2, 0, 0);
                /* the pointer: a phi of the header, stepped at the end of the latch */
                pidx = opt_phi_new(hd, opt_vreg(IR_I64, -1));
                phi = opt_I[pidx];
                ptr = opt_vreg(IR_I64, -1);
                opt_I[pidx].dst = ptr;
                opt_I[pidx].aux = ptr;
                opt_iroom();
                opt_blk[pidx] = hd;
                opt_def[ptr] = pidx;
                pnext = opt_new_inst(latch, opt_B[latch].n - 1, IR_ADD, IR_I64, ptr, -1, (long)((unsigned long)step * (unsigned long)c), IRF_IMM);
                opt_P[phi.imm].v[kpre] = p0;
                opt_P[phi.imm].v[klatch] = pnext;
                rep[nrep].base = base;
                rep[nrep].iv = iv;
                rep[nrep].c = c;
                rep[nrep].ptr = ptr;
                nrep++;
                any = 1;
                /* the header and the latch moved on: find this add again */
                for (j = 0; j < opt_B[b].n && opt_B[b].code[j] != ii; j++) {
                }
            }
            /* every use of the address takes the pointer instead */
            opt_repl_all(opt_I[ii].dst, rep[r].ptr);
            opt_kill(&opt_I[ii]);
        }
    }
    return any;
}

/*
 * The loops that make no calls (which would keep the hoisted values in
 * callee-saved registers, or spilled, across the whole loop) move their
 * invariant computations to a block in front of the header: one made
 * on the edge from the only block entering the loop.
 */
static int opt_licm(void) {
    int *hdrs = xmalloc((size_t)(opt_nb + 1) * sizeof(int));
    int nh = 0, h, k, j, m, any = 0;
    char *in;
    int *work;

    opt_hoisted = opt_grow(opt_hoisted, &opt_hcap, opt_ni + 1, 1);
    memset(opt_hoisted, 0, (size_t)opt_hcap);
    for (k = 0; k < opt_nrpo; k++) {
        int b = opt_rpo[k];
        for (j = 0; j < opt_B[b].npreds; j++) {
            if (opt_dominates(b, opt_B[b].preds[j])) {
                hdrs[nh++] = b;
                break;
            }
        }
    }
    /* room for the preheaders, one per loop */
    in = xcalloc((size_t)(opt_nb + nh) + 1, 1);
    work = xmalloc((size_t)(opt_nb + nh + 1) * sizeof(int));
    /* innermost first: from the last header in reverse postorder */
    for (h = nh - 1; h >= 0; h--) {
        int hd = hdrs[h], nw = 0, ninst = 0, outside = -1, nout = 0, calls = 0, pre, e, n;
        IRInst br;
        memset(in, 0, (size_t)opt_nb + 1);
        in[hd] = 1;
        for (j = 0; j < opt_B[hd].npreds; j++) {
            int p = opt_B[hd].preds[j];
            if (opt_dominates(hd, p) && !in[p]) {
                in[p] = 1;
                work[nw++] = p;
            }
        }
        while (nw) {
            int b = work[--nw];
            for (j = 0; j < opt_B[b].npreds; j++) {
                int p = opt_B[b].preds[j];
                if (!in[p]) {
                    in[p] = 1;
                    work[nw++] = p;
                }
            }
        }
        for (k = 0; k < opt_nb; k++) {
            if (!in[k]) {
                continue;
            }
            for (j = 0; j < opt_B[k].n; j++) {
                const IRInst *i = &opt_I[opt_B[k].code[j]];
                ninst++;
                calls += ir_clobbers_registers(opt_F, i) || i->op == IR_VAARG || i->op == IR_VASTART;
            }
        }
        for (j = 0; j < opt_B[hd].npreds; j++) {
            if (!in[opt_B[hd].preds[j]]) {
                outside = j;
                nout++;
            }
        }
        if (calls || ninst > 2000 || nout != 1) {
            continue;
        }
        /* the preheader: a new block on the entering edge */
        pre = opt_block_new(opt_B[hd].preds[outside]);
        memset(&br, 0, sizeof(br));
        br.op = IR_BR;
        br.dst = br.a = br.b = br.aux = -1;
        br.imm = hd;
        opt_push(pre, opt_inst(&br));
        opt_iroom();
        opt_hoisted = opt_grow(opt_hoisted, &opt_hcap, opt_ni + 1, 1);
        {
            int p = opt_B[hd].preds[outside];
            n = opt_nsucc(p);
            for (e = 0; e < n && opt_succ(p, e) != hd; e++) {
            }
            ir_set_succ(opt_F, opt_term(p), e, pre);
            opt_add_pred(pre, p);
            opt_B[hd].preds[outside] = pre;
        }
        /* what to hoist, in reverse postorder so that operands go first */
        for (k = 0; k < opt_nrpo; k++) {
            int b = opt_rpo[k];
            if (!in[b]) {
                continue;
            }
            for (j = 0; j < opt_B[b].n; j++) {
                int ii = opt_B[b].code[j];
                IRInst *i = &opt_I[ii];
                int ok = opt_hoistable(i);
                for (m = 0; ok && m < 2; m++) {
                    int u = m == 0 ? i->a : i->b;
                    if (u >= 0 && opt_def[u] >= 0 && opt_blk[opt_def[u]] >= 0 && in[opt_blk[opt_def[u]]]) {
                        ok = 0;
                    }
                }
                if (!ok) {
                    continue;
                }
                memmove(&opt_B[b].code[j], &opt_B[b].code[j + 1], (size_t)(opt_B[b].n - j - 1) * sizeof(int));
                opt_B[b].n--;
                j--;
                opt_insert(pre, opt_B[pre].n - 1, ii);
                opt_blk[ii] = pre;
                opt_hoisted[ii] = 1;
                any = 1;
            }
        }
        any |= opt_reduce(hd, pre, in);
        /* the dominators changed with the new block */
        opt_dominators();
    }
    free(in);
    free(work);
    free(hdrs);
    return any;
}

/*
 * A 32 bit load whose only use extends it to 64 bits loads 64 bits
 * itself: the arm64 and x86_64 loads extend for free (zero extending a
 * zero extended value, or sign extending a sign extended one).
 */
static void opt_widen_loads(void) {
    int k, j;
    opt_index();
    for (k = 0; k < opt_nrpo; k++) {
        OptBlock *ob = &opt_B[opt_rpo[k]];
        for (j = 0; j < ob->n; j++) {
            IRInst *x = &opt_I[ob->code[j]];
            IRInst *l;
            int signed_load;
            if ((x->op != IR_SEXT && x->op != IR_ZEXT) || x->type != IR_I64 || x->size != 4 || x->a < 0
                    || opt_vtype(x->a) != IR_I32 || opt_nuse[x->a] != 1 || opt_def[x->a] < 0) {
                continue;
            }
            l = &opt_I[opt_def[x->a]];
            if (l->op != IR_LOAD || (l->flags & IRF_VOLATILE) || l->type != IR_I32) {
                continue;
            }
            signed_load = (l->flags & IRF_SIGNED) != 0;
            /* zext of a narrower unsigned load, or sext of a signed one; a full word either way when the signs agree */
            if (x->op == IR_ZEXT ? (signed_load && l->size < 4) || (signed_load && l->size == 4) : !signed_load && l->size == 4) {
                continue;
            }
            if (x->op == IR_SEXT && !signed_load) {
                continue;
            }
            l->type = IR_I64;
            l->dst = x->dst;
            opt_def[x->dst] = opt_def[x->a];
            opt_kill(x);
        }
    }
    opt_compact();
}

/* the instruction ii can move to just before its only user, which the backends then fuse it with */
static int opt_sink_target(int ii) {
    const IRInst *i = &opt_I[ii];
    const IRInst *u;
    int user;
    if (i->dst < 0 || opt_nuse[i->dst] != 1) {
        return -1;
    }
    user = opt_user[i->dst];
    if (user < 0 || opt_blk[user] != opt_blk[ii] || opt_pos[user] <= opt_pos[ii]) {
        return -1;
    }
    u = &opt_I[user];
    if ((i->op == IR_ICMP || i->op == IR_FCMP) && u->op == IR_CONDBR) {
        return user;
    }
    if ((i->op == IR_ADDR_LOCAL || (i->op == IR_ADD && (i->flags & IRF_IMM) && i->type == IR_I64))
            && (u->op == IR_LOAD || u->op == IR_STORE || u->op == IR_LOADBITS || u->op == IR_STOREBITS)
            && u->a == i->dst && u->b != i->dst) {
        return user;
    }
    return -1;
}

/* a constant or address that is as cheap to compute again as to keep in a register */
static int opt_cheap(const IRInst *d) {
    return (d->op == IR_ICONST || d->op == IR_ADDR_LOCAL || d->op == IR_ADDR_GLOBAL || d->op == IR_ADDR_STRING
            || (d->op == IR_FCONST && d->type != IR_F80)) && d->dst >= 0 && opt_is_var(d->dst);
}

/* does instruction i read v */
static int opt_reads(IRInst *i, int v) {
    int m, n = opt_nops(i);
    for (m = 0; m < n; m++) {
        if (*opt_op(i, m) == v) {
            return 1;
        }
    }
    return 0;
}

/*
 * A computation from one register (x + 4, a conversion) moves down past
 * the other reads of that register in its block, so that the register
 * dies where the new value is born and the two can share a register:
 * `p' = p + 4; load [p]` (C's *p++) keeps both alive, `load [p]; p' =
 * p + 4` does not.
 */
static void opt_sink_past_uses(int b) {
    OptBlock *ob = &opt_B[b];
    int j, k, a, last, first, ii;
    for (j = ob->n - 2; j >= 0; j--) {
        IRInst *d = &opt_I[ob->code[j]];
        if (!opt_pure(d) || !opt_all_vars(d) || d->a < 0 || d->b >= 0 || opt_cheap(d)) {
            continue;
        }
        a = d->a;
        last = first = -1;
        for (k = j + 1; k < ob->n && k <= j + 64; k++) {
            IRInst *u = &opt_I[ob->code[k]];
            if (first < 0 && opt_reads(u, d->dst)) {
                first = k;
            }
            if (opt_reads(u, a)) {
                last = k;
            }
        }
        if (last < 0 || last >= ob->n - 1 || (first >= 0 && first <= last)) {
            continue;
        }
        ii = ob->code[j];
        memmove(&ob->code[j], &ob->code[j + 1], (size_t)(last - j) * sizeof(int));
        ob->code[last] = ii;
    }
}

/*
 * A value carried around a loop (the argument of a phi on the edge from
 * its block) is computed after the last read of that phi in the block
 * when nothing reads it earlier, so that the old value dies where the
 * new one is born and the two coalesce into one register: no copy on
 * the back edge (`zr' = zr*zr - zi*zi + cr; zi' = 2*zr*zi + ci`).
 */
static void opt_sink_carried(void) {
    int k, j, m, e;
    opt_index();
    for (k = 0; k < opt_nrpo; k++) {
        OptBlock *sb = &opt_B[opt_rpo[k]];
        for (j = 0; j < sb->n && opt_I[sb->code[j]].op == OPT_PHI; j++) {
            IRInst *q = &opt_I[sb->code[j]];
            for (e = 0; e < sb->npreds; e++) {
                int v = opt_P[q->imm].v[e];
                OptBlock *ob;
                int d, pos, last = -1, first = -1;
                if (v < 0 || (d = opt_def[v]) < 0 || opt_I[d].op == OPT_PHI || !opt_pure(&opt_I[d]) || !opt_all_vars(&opt_I[d])) {
                    continue;
                }
                ob = &opt_B[opt_blk[d]];
                pos = opt_pos[d];
                if (ob->code[pos] != d) {
                    continue;   /* moved already this pass */
                }
                for (m = pos + 1; m < ob->n && m <= pos + 64; m++) {
                    IRInst *u = &opt_I[ob->code[m]];
                    if (first < 0 && opt_reads(u, v)) {
                        first = m;
                    }
                    if (opt_reads(u, q->dst)) {
                        last = m;
                    }
                }
                if (last < 0 || last >= ob->n - 1 || (first >= 0 && first <= last)) {
                    continue;
                }
                memmove(&ob->code[pos], &ob->code[pos + 1], (size_t)(last - pos) * sizeof(int));
                ob->code[last] = d;
                for (m = pos; m <= last; m++) {
                    opt_pos[ob->code[m]] = m;
                }
            }
        }
    }
}

static void opt_sink(void) {
    int k, j, n;
    int *buf = NULL;
    int *head, *next, *tail;
    int cap = 0;
    opt_sink_carried();
    for (k = 0; k < opt_nrpo; k++) {
        opt_sink_past_uses(opt_rpo[k]);
    }
    opt_index();
    head = xmalloc((size_t)(opt_ni + 1) * sizeof(int));
    next = xmalloc((size_t)(opt_ni + 1) * sizeof(int));
    tail = xmalloc((size_t)(opt_ni + 1) * sizeof(int));
    for (k = 0; k < opt_nrpo; k++) {
        OptBlock *ob = &opt_B[opt_rpo[k]];
        buf = opt_grow(buf, &cap, ob->n + 1, sizeof(int));
        for (j = 0; j < ob->n; j++) {
            head[ob->code[j]] = tail[ob->code[j]] = -1;
        }
        /* each user's sinking instructions, in their order */
        for (j = 0; j < ob->n; j++) {
            int ii = ob->code[j];
            int u = opt_sink_target(ii);
            next[ii] = -1;
            if (u < 0) {
                continue;
            }
            if (tail[u] < 0) {
                head[u] = ii;
            } else {
                next[tail[u]] = ii;
            }
            tail[u] = ii;
        }
        n = 0;
        for (j = 0; j < ob->n; j++) {
            int ii = ob->code[j];
            int s;
            if (opt_sink_target(ii) >= 0) {
                continue;
            }
            for (s = head[ii]; s >= 0; s = next[s]) {
                buf[n++] = s;
            }
            buf[n++] = ii;
        }
        memcpy(ob->code, buf, (size_t)n * sizeof(int));
        ob->n = n;
    }
    free(buf);
    free(head);
    free(next);
    free(tail);
}

/*
 * Rematerialization: a constant or address read in another block than
 * the one computing it is computed again just before the reader, as at
 * -O0, rather than held in a register all the way there (the allocator
 * cannot split a live range). Phi arguments are handled when the copies
 * out of SSA are made.
 */
static void opt_remat(void) {
    int k, j, m, n;
    opt_index();
    for (k = 0; k < opt_F->nvregs; k++) {
        opt_vstamp[k] = -1;
    }
    for (k = 0; k < opt_nrpo; k++) {
        int b = opt_rpo[k];
        for (j = 0; j < opt_B[b].n; j++) {
            int ii = opt_B[b].code[j];
            IRInst *i = &opt_I[ii];
            if (i->op == OPT_PHI) {
                continue;
            }
            n = opt_nops(i);
            for (m = 0; m < n; m++) {
                int u = *opt_op(&opt_I[ii], m);
                int d, c;
                /* a call's argument is computed right before the call, where the backend can put it straight into its register */
                int arg = opt_I[ii].op == IR_CALL && m >= 5;     /* through the index: an insertion may have moved the pool */
                if (u < 0 || (d = opt_def[u]) < 0 || (opt_blk[d] == b && !arg) || !opt_cheap(&opt_I[d])
                        || (opt_hoisted && d < opt_hcap && opt_hoisted[d])) {
                    continue;
                }
                if (arg) {
                    IRInst copy = opt_I[d];
                    c = opt_vreg(opt_vtype(u), u);
                    copy.dst = c;
                    opt_insert(b, j, opt_inst(&copy));
                    j++;
                } else if (opt_vstamp[u] == b) {
                    c = opt_vaux[u];        /* already computed again in this block */
                } else {
                    IRInst copy = opt_I[d];
                    c = opt_vreg(opt_vtype(u), u);
                    copy.dst = c;
                    opt_insert(b, j, opt_inst(&copy));
                    j++;
                    opt_vstamp[u] = b;
                    opt_vaux[u] = c;
                }
                *opt_op(&opt_I[ii], m) = c;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* out of SSA                                                         */
/* ------------------------------------------------------------------ */

static OptWord *opt_livein = NULL;
static OptWord *opt_liveout = NULL;
static int opt_nw = 0;
static int *opt_cls = NULL;     /* union-find parent */
static int *opt_cnext = NULL;   /* the next member of the class, circular */
static int *opt_csize = NULL;

static int opt_has(const OptWord *s, int v) {
    return (int)((s[v / OPT_WBITS] >> (v % OPT_WBITS)) & 1);
}

static void opt_liveness(void) {
    int changed, k, j, m, n, w, s2, kk;
    OptWord *live;
    size_t bytes;
    opt_nw = (opt_F->nvregs + OPT_WBITS) / OPT_WBITS;
    bytes = (size_t)opt_nw * sizeof(OptWord);
    opt_livein = xrealloc(opt_livein, (size_t)(opt_nb + 1) * bytes);
    opt_liveout = xrealloc(opt_liveout, (size_t)(opt_nb + 1) * bytes);
    memset(opt_livein, 0, (size_t)(opt_nb + 1) * bytes);
    memset(opt_liveout, 0, (size_t)(opt_nb + 1) * bytes);
    live = xmalloc(bytes);
    do {
        changed = 0;
        for (k = opt_nrpo - 1; k >= 0; k--) {
            int b = opt_rpo[k];
            OptBlock *ob = &opt_B[b];
            OptWord *out = &opt_liveout[(size_t)b * (size_t)opt_nw];
            OptWord *in = &opt_livein[(size_t)b * (size_t)opt_nw];
            n = opt_nsucc(b);
            memset(live, 0, bytes);
            for (kk = 0; kk < n; kk++) {
                OptBlock *sb = &opt_B[opt_succ(b, kk)];
                const OptWord *sin = &opt_livein[(size_t)opt_succ(b, kk) * (size_t)opt_nw];
                if (!opt_first_edge(b, kk)) {
                    continue;
                }
                for (w = 0; w < opt_nw; w++) {
                    live[w] |= sin[w];
                }
                for (s2 = 0; s2 < sb->npreds; s2++) {
                    if (sb->preds[s2] != b) {
                        continue;
                    }
                    for (j = 0; j < sb->n; j++) {
                        const IRInst *phi = &opt_I[sb->code[j]];
                        int v;
                        if (phi->op != OPT_PHI) {
                            break;
                        }
                        v = opt_P[phi->imm].v[s2];
                        if (v >= 0) {
                            live[v / OPT_WBITS] |= 1UL << (v % OPT_WBITS);
                        }
                    }
                }
            }
            if (memcmp(out, live, bytes) != 0) {
                memcpy(out, live, bytes);
            }
            for (j = ob->n - 1; j >= 0; j--) {
                IRInst *i = &opt_I[ob->code[j]];
                if (i->dst >= 0) {
                    live[i->dst / OPT_WBITS] &= ~(1UL << (i->dst % OPT_WBITS));
                }
                if (i->op == OPT_PHI) {
                    continue;
                }
                n = opt_nops(i);
                for (m = 0; m < n; m++) {
                    int u = *opt_op(i, m);
                    if (u >= 0) {
                        live[u / OPT_WBITS] |= 1UL << (u % OPT_WBITS);
                    }
                }
            }
            if (memcmp(in, live, bytes) != 0) {
                memcpy(in, live, bytes);
                changed = 1;
            }
        }
    } while (changed);
    free(live);
}

/* where v is defined: its block and position; a phi is at -1 and a parameter at -2 of the entry */
static void opt_defsite(int v, int *b, int *p) {
    int d = opt_def[v];
    if (d < 0) {
        *b = 0;
        *p = -2;
        return;
    }
    *b = opt_blk[d];
    *p = opt_I[d].op == OPT_PHI ? -1 : opt_pos[d];
}

/* v is live just after position p of block b */
static int opt_live_after(int v, int b, int p) {
    int j, m, n;
    const OptBlock *ob = &opt_B[b];
    if (p < 0) {
        return opt_has(&opt_livein[(size_t)b * (size_t)opt_nw], v);
    }
    if (opt_has(&opt_liveout[(size_t)b * (size_t)opt_nw], v)) {
        return 1;
    }
    for (j = p + 1; j < ob->n; j++) {
        IRInst *i = &opt_I[ob->code[j]];
        n = opt_nops(i);
        for (m = 0; m < n; m++) {
            if (*opt_op(i, m) == v) {
                return 1;
            }
        }
    }
    return 0;
}

static int opt_interfere(int a, int b) {
    int ba, pa, bb, pb;
    opt_defsite(a, &ba, &pa);
    opt_defsite(b, &bb, &pb);
    if (ba == bb) {
        if (pa == pb) {
            return 1;
        }
        return pa < pb ? opt_live_after(a, bb, pb) : opt_live_after(b, ba, pa);
    }
    if (opt_dominates(ba, bb)) {
        return opt_live_after(a, bb, pb);
    }
    if (opt_dominates(bb, ba)) {
        return opt_live_after(b, ba, pa);
    }
    return 0;
}

static int opt_find(int v) {
    while (opt_cls[v] != v) {
        opt_cls[v] = opt_cls[opt_cls[v]];
        v = opt_cls[v];
    }
    return v;
}

static int opt_is_param(int v) {
    int k;
    for (k = 0; k < opt_F->nparams; k++) {
        if (opt_F->params[k].vreg == v) {
            return 1;
        }
    }
    return 0;
}

/* merge the classes of x and y unless two of their members interfere */
static void opt_try_union(int x, int y) {
    int a = opt_find(x), b = opt_find(y), p, q, t;
    long pairs;
    if (a == b || opt_vtype(a) != opt_vtype(b)) {
        return;
    }
    pairs = (long)opt_csize[a] * opt_csize[b];
    if (pairs > 20000) {
        return;
    }
    p = a;
    do {
        q = b;
        do {
            if (opt_interfere(p, q)) {
                return;
            }
            q = opt_cnext[q];
        } while (q != b);
        p = opt_cnext[p];
    } while (p != a);
    /* a parameter stays its class's register, so that IRParam.vreg still names it */
    if (opt_vflag[b] && !opt_vflag[a]) {
        t = a;
        a = b;
        b = t;
    }
    opt_cls[b] = a;
    opt_csize[a] += opt_csize[b];
    t = opt_cnext[a];
    opt_cnext[a] = opt_cnext[b];
    opt_cnext[b] = t;
}

/*
 * Split the edges from a block with several successors into a block
 * with phis, however many predecessors it has: the copies for the phis
 * go at the end of the predecessor, where they would also run on the
 * way to its other successors. (A phi with one predecessor is normally
 * simplified away, but the simplification rounds are capped.)
 */
static void opt_split_edges(void) {
    int b, k, nb0 = opt_nb;
    for (b = 0; b < nb0; b++) {
        OptBlock *ob = &opt_B[b];
        if (!ob->live || ob->n == 0 || opt_I[ob->code[0]].op != OPT_PHI) {
            continue;
        }
        for (k = 0; k < opt_B[b].npreds; k++) {
            int p = opt_B[b].preds[k];
            int n = opt_nsucc(p);
            int nb, e;
            IRInst br;
            if (n < 2) {
                continue;
            }
            nb = opt_block_new(p);
            memset(&br, 0, sizeof(br));
            br.op = IR_BR;
            br.dst = br.a = br.b = br.aux = -1;
            br.imm = b;
            opt_push(nb, opt_inst(&br));
            opt_add_pred(nb, p);
            /* the first of p's edges still going to b (a switch may have several) */
            for (e = 0; e < n && opt_succ(p, e) != b; e++) {
            }
            ir_set_succ(opt_F, opt_term(p), e, nb);
            opt_B[b].preds[k] = nb;
        }
    }
}

/* src is a constant or address the copy computes again rather than reads */
static int opt_remat_src(int src) {
    return opt_def[src] >= 0 && opt_cheap(&opt_I[opt_def[src]]);
}

/* emit the parallel copy dst[k] = src[k] (k < n) as moves before the terminator of block b */
static void opt_parallel_copy(int b, int *dst, int *src, int n) {
    IRInst mv;
    memset(&mv, 0, sizeof(mv));
    mv.op = IR_MOV;
    mv.b = mv.aux = -1;
    while (n > 0) {
        int k, j, done = 0;
        for (k = 0; k < n; k++) {
            /* dst[k] may be written once no other pending copy reads it */
            for (j = 0; j < n; j++) {
                if (j != k && src[j] == dst[k] && !opt_remat_src(src[j])) {
                    break;
                }
            }
            if (j == n) {
                if (opt_remat_src(src[k])) {
                    IRInst copy = opt_I[opt_def[src[k]]];
                    copy.dst = dst[k];
                    opt_insert(b, opt_B[b].n - 1, opt_inst(&copy));
                } else {
                    mv.dst = dst[k];
                    mv.a = src[k];
                    mv.type = (unsigned char)opt_vtype(dst[k]);
                    opt_insert(b, opt_B[b].n - 1, opt_inst(&mv));
                }
                dst[k] = dst[n - 1];
                src[k] = src[n - 1];
                n--;
                done = 1;
                break;
            }
        }
        if (!done) {
            /* a cycle: save one destination, then read the saved copy */
            int t = opt_vreg(opt_vtype(dst[0]), dst[0]);
            int old = dst[0];
            mv.dst = t;
            mv.a = old;
            mv.type = (unsigned char)opt_vtype(old);
            opt_insert(b, opt_B[b].n - 1, opt_inst(&mv));
            for (j = 0; j < n; j++) {
                if (src[j] == old) {
                    src[j] = t;
                }
            }
        }
    }
}

static void opt_leave_ssa(void) {
    int k, j, m, b, v, n, nv;
    int *dst = NULL, *src = NULL;
    int cap = 0;

    opt_split_edges();
    opt_dominators();
    opt_index();
    opt_liveness();
    nv = opt_F->nvregs;
    opt_cls = xrealloc(opt_cls, (size_t)(nv + 1) * sizeof(int));
    opt_cnext = xrealloc(opt_cnext, (size_t)(nv + 1) * sizeof(int));
    opt_csize = xrealloc(opt_csize, (size_t)(nv + 1) * sizeof(int));
    for (v = 0; v < nv; v++) {
        opt_cls[v] = v;
        opt_cnext[v] = v;
        opt_csize[v] = 1;
        opt_vflag[v] = (char)opt_is_param(v);
    }
    /* coalesce each phi with its arguments */
    for (k = 0; k < opt_nrpo; k++) {
        OptBlock *ob = &opt_B[opt_rpo[k]];
        for (j = 0; j < ob->n; j++) {
            IRInst *i = &opt_I[ob->code[j]];
            const OptPhi *ph;
            if (i->op != OPT_PHI) {
                break;
            }
            ph = &opt_P[i->imm];
            for (m = 0; m < ph->n; m++) {
                /* a constant argument is computed again at the end of the predecessor instead, and an undefined one not at all */
                if (ph->v[m] >= 0 && !opt_remat_src(ph->v[m]) && !opt_is_undef(ph->v[m])) {
                    opt_try_union(i->dst, ph->v[m]);
                }
            }
        }
    }
    /* copies for the rest, at the end of each predecessor */
    for (k = 0; k < opt_nrpo; k++) {
        b = opt_rpo[k];
        for (m = 0; m < opt_B[b].npreds; m++) {
            int nc = 0;
            for (j = 0; j < opt_B[b].n; j++) {
                IRInst *i = &opt_I[opt_B[b].code[j]];
                int d, s;
                if (i->op != OPT_PHI) {
                    break;
                }
                d = opt_find(i->dst);
                s = opt_P[i->imm].v[m] >= 0 && !opt_is_undef(opt_P[i->imm].v[m]) ? opt_find(opt_P[i->imm].v[m]) : d;
                if (opt_P[i->imm].v[m] >= 0 && opt_is_undef(opt_P[i->imm].v[m])) {
                    opt_vflag[d] |= 2;      /* no copy for an undefined argument: the register may be read on a path that never wrote it */
                }
                if (d != s) {
                    dst = opt_grow(dst, &cap, nc + 1, sizeof(int));
                    src = xrealloc(src, (size_t)cap * sizeof(int));
                    dst[nc] = d;
                    src[nc] = s;
                    nc++;
                }
            }
            if (nc) {
                opt_parallel_copy(opt_B[b].preds[m], dst, src, nc);
            }
        }
    }
    free(dst);
    free(src);
    /* every register to its class's, and the phis gone */
    for (k = 0; k < opt_nrpo; k++) {
        OptBlock *ob = &opt_B[opt_rpo[k]];
        for (j = 0; j < ob->n; j++) {
            IRInst *i = &opt_I[ob->code[j]];
            if (i->op == OPT_PHI) {
                opt_kill(i);
                continue;
            }
            n = opt_nops(i);
            for (m = 0; m < n; m++) {
                int *p = opt_op(i, m);
                if (*p >= 0 && *p < nv) {
                    *p = opt_find(*p);
                }
            }
            if (i->dst >= 0 && i->dst < nv) {
                i->dst = opt_find(i->dst);
            }
        }
    }
    opt_compact();
    /* a class takes a member's name for the dump */
    for (v = 0; v < nv; v++) {
        int r = opt_find(v);
        if (!opt_F->vregs[r].name && opt_F->vregs[v].name) {
            opt_F->vregs[r].name = opt_F->vregs[v].name;
        }
    }
}

/* ------------------------------------------------------------------ */
/* the CFG after SSA                                                  */
/* ------------------------------------------------------------------ */

/* where a jump to b ends up when blocks that only jump are passed through */
static int opt_final_target(int b) {
    int hops = 0;
    while (hops++ < 64 && b != 0 && opt_B[b].n == 1 && opt_I[opt_B[b].code[0]].op == IR_BR && (int)opt_I[opt_B[b].code[0]].imm != b) {
        b = (int)opt_I[opt_B[b].code[0]].imm;
    }
    return b;
}

static void opt_cleanup_cfg(void) {
    int b, k, changed, iterations = 0;
    do {
        changed = 0;
        for (b = 0; b < opt_nb; b++) {
            IRInst *t;
            if (!opt_B[b].live || opt_B[b].n == 0) {
                continue;
            }
            t = opt_term(b);
            for (k = 0; k < opt_nsucc(b); k++) {
                int f = opt_final_target(opt_succ(b, k));
                if (f != opt_succ(b, k)) {
                    ir_set_succ(opt_F, t, k, f);
                    changed = 1;
                }
            }
            if (t->op == IR_CONDBR) {
                if (t->imm == t->aux) {
                    t->op = IR_BR;
                    t->a = -1;
                    t->type = IR_VOID;
                    changed = 1;
                }
            }
        }
        opt_prune();
        opt_compute_preds();
        /* a block whose only predecessor jumps to it joins that predecessor */
        for (b = 0; b < opt_nb; b++) {
            OptBlock *ob = &opt_B[b];
            int s, j;
            if (!ob->live || ob->n == 0) {
                continue;
            }
            while (opt_term(b)->op == IR_BR) {
                s = (int)opt_term(b)->imm;
                if (s == b || s == 0 || opt_B[s].npreds != 1) {
                    break;
                }
                ob = &opt_B[b];
                ob->n--;
                for (j = 0; j < opt_B[s].n; j++) {
                    opt_push(b, opt_B[s].code[j]);
                }
                /* s's successors now have b for a predecessor instead */
                {
                    int nt = opt_nsucc(b), q, r;
                    for (q = 0; q < nt; q++) {
                        int t2 = opt_succ(b, q);
                        if (!opt_first_edge(b, q)) {
                            continue;
                        }
                        for (r = 0; r < opt_B[t2].npreds; r++) {
                            if (opt_B[t2].preds[r] == s) {
                                opt_B[t2].preds[r] = b;
                            }
                        }
                    }
                }
                opt_B[s].n = 0;
                opt_B[s].live = 0;
                opt_B[s].npreds = 0;
                changed = 1;
                ob = &opt_B[b];
            }
        }
    } while (changed && ++iterations < 8);
}

/* remove computations whose register nothing reads, now that registers may have several definitions */
static void opt_dce_flat(void) {
    int b, j, k, m, n, again, rounds = 0;
    int *uses = xmalloc((size_t)(opt_F->nvregs + 1) * sizeof(int));
    do {
        again = 0;
        for (k = 0; k < opt_F->nvregs; k++) {
            uses[k] = 0;
        }
        for (b = 0; b < opt_nb; b++) {
            if (!opt_B[b].live) {
                continue;
            }
            for (j = 0; j < opt_B[b].n; j++) {
                IRInst *i = &opt_I[opt_B[b].code[j]];
                n = opt_nops(i);
                for (m = 0; m < n; m++) {
                    int u = *opt_op(i, m);
                    if (u >= 0) {
                        uses[u]++;
                    }
                }
            }
        }
        for (b = 0; b < opt_nb; b++) {
            if (!opt_B[b].live) {
                continue;
            }
            for (j = 0; j < opt_B[b].n; j++) {
                IRInst *i = &opt_I[opt_B[b].code[j]];
                if (i->op != IR_NOP && i->dst >= 0 && uses[i->dst] == 0 && opt_removable(i) && !opt_is_param(i->dst)) {
                    opt_kill(i);
                    again = 1;
                }
                if (i->op == IR_MOV && i->dst == i->a) {
                    opt_kill(i);
                    again = 1;
                }
            }
        }
        opt_compact();
    } while (again && ++rounds < 4);
    free(uses);
}

/* number the registers densely, in their order, and mark the ones written more than once */
static void opt_renumber(void) {
    int nv = opt_F->nvregs;
    int *map = xmalloc((size_t)(nv + 1) * sizeof(int));
    int *ndefs = xcalloc((size_t)nv + 1, sizeof(int));
    IRVReg *vregs;
    int b, j, k, m, n, v, count = 0;
    for (v = 0; v < nv; v++) {
        map[v] = -1;
    }
    for (k = 0; k < opt_F->nparams; k++) {
        if (opt_F->params[k].vreg >= 0) {
            /* the prologue's write counts: a parameter assigned in the body has two (the backends fold a single-definition address) */
            map[opt_F->params[k].vreg] = 0;
            ndefs[opt_F->params[k].vreg]++;
        }
    }
    for (b = 0; b < opt_nb; b++) {
        if (!opt_B[b].live) {
            continue;
        }
        for (j = 0; j < opt_B[b].n; j++) {
            IRInst *i = &opt_I[opt_B[b].code[j]];
            n = opt_nops(i);
            for (m = 0; m < n; m++) {
                int u = *opt_op(i, m);
                if (u >= 0) {
                    map[u] = 0;
                }
            }
            if (i->dst >= 0) {
                map[i->dst] = 0;
                ndefs[i->dst]++;
            }
        }
    }
    vregs = xmalloc((size_t)(nv + 1) * sizeof(IRVReg));
    for (v = 0; v < nv; v++) {
        if (map[v] < 0) {
            continue;
        }
        map[v] = count;
        vregs[count] = opt_F->vregs[v];
        vregs[count].is_local = ndefs[v] > 1 || (opt_vflag[v] & 2);
        count++;
    }
    for (b = 0; b < opt_nb; b++) {
        if (!opt_B[b].live) {
            continue;
        }
        for (j = 0; j < opt_B[b].n; j++) {
            IRInst *i = &opt_I[opt_B[b].code[j]];
            n = opt_nops(i);
            for (m = 0; m < n; m++) {
                int *p = opt_op(i, m);
                if (*p >= 0) {
                    *p = map[*p];
                }
            }
            if (i->dst >= 0) {
                i->dst = map[i->dst];
            }
        }
    }
    for (k = 0; k < opt_F->nparams; k++) {
        if (opt_F->params[k].vreg >= 0) {
            opt_F->params[k].vreg = map[opt_F->params[k].vreg];
        }
    }
    free(opt_F->vregs);
    opt_F->vregs = vregs;
    opt_F->nvregs = count;
    opt_F->vregs_cap = nv + 1;
    free(map);
    free(ndefs);
}

/* ------------------------------------------------------------------ */
/* threading before SSA                                               */
/* ------------------------------------------------------------------ */

/* the constant register v holds at the end of block p, as far as p's own code says */
static int opt_flag_at_end(int p, int v, long *x) {
    int j, depth = 0;
    for (j = opt_B[p].n - 2; j >= 0 && depth < 4; j--) {
        const IRInst *i = &opt_I[opt_B[p].code[j]];
        if (i->dst != v) {
            continue;
        }
        if (i->op == IR_ICONST) {
            *x = i->imm;
            return 1;
        }
        if (i->op != IR_MOV || i->a < 0) {
            return 0;
        }
        v = i->a;       /* a copy: what was copied */
        depth++;
    }
    return 0;
}

/*
 * A block that is only a branch on a register, entered by jumps from
 * blocks that have just set that register to a constant: each of those
 * jumps straight to where the branch would go. This undoes the flag
 * that lowering makes for a conditional expression used as a condition
 * (`if (a ? (x = y, 1) : 0)`, the shape of many macros), before SSA
 * would merge a value that the paths through the flag's 0 never set.
 */
static void opt_thread_flags(void) {
    int b, k, any = 0;
    for (b = 1; b < opt_nb; b++) {
        IRInst *t;
        if (!opt_B[b].live || opt_B[b].n != 1) {
            continue;
        }
        t = opt_term(b);
        if (t->op != IR_CONDBR || t->a < 0) {
            continue;
        }
        for (k = 0; k < opt_B[b].npreds; k++) {
            int p = opt_B[b].preds[k];
            IRInst *pt = opt_term(p);
            long x;
            int target;
            if (pt->op != IR_BR || !opt_flag_at_end(p, t->a, &x)) {
                continue;
            }
            target = x ? (int)t->imm : t->aux;
            if (target != b) {
                pt->imm = target;
                any = 1;
            }
        }
    }
    if (any) {
        opt_prune();
        opt_compute_preds();
    }
}

/* ------------------------------------------------------------------ */
/* inlining                                                           */
/* ------------------------------------------------------------------ */

#define OPT_INLINE_SMALL 48     /* instructions: a callee this small is inlined at every call */
#define OPT_INLINE_ONCE 400     /* a static callee called once, which then disappears */
#define OPT_INLINE_GROWTH 12000 /* a caller stops taking callees beyond this many instructions */

static IRFunc **opt_unit_f = NULL;      /* the translation unit's functions, while opt_unit runs */
static int opt_unit_n = 0;
static int *opt_unit_sorted = NULL;     /* their indices, by name */
static int *opt_unit_state = NULL;      /* 0 not optimized yet, 1 being optimized, 2 done */
static int *opt_unit_calls = NULL;      /* the calls to each */
static const char *opt_unit_drop = NULL;

static int opt_unit_find(const char *name) {
    int lo = 0, hi = opt_unit_n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(name, opt_unit_f[opt_unit_sorted[mid]]->name);
        if (c == 0) {
            return opt_unit_sorted[mid];
        }
        if (c < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return -1;
}

/*
 * A function's size for inlining: its instructions, less those that cost
 * nothing once selected (jumps, constants, copies, offsets folded into
 * addresses), and more for each call it makes, whose argument moves and
 * the registers it clobbers add to the caller's pressure (clang's inliner
 * has the same penalty, so a function that is mostly calls stays one).
 */
static int opt_inst_count(const IRFunc *f) {
    int i, n = 0;
    for (i = 0; i < f->ninsts; i++) {
        const IRInst *in = &f->insts[i];
        if (in->op == IR_NOP || in->op == IR_BR || in->op == IR_ICONST || in->op == IR_MOV || in->op == IR_ADDR_LOCAL
                || (in->op == IR_ADD && (in->flags & IRF_IMM) && in->type == IR_I64)) {
            continue;
        }
        n += in->op == IR_CALL ? 3 : 1;
    }
    return n;
}

/* the defined function a call can be replaced by, or NULL */
static IRFunc *opt_inlinable(const IRInst *call, int caller_size) {
    const IRCall *c = &opt_F->calls[call->imm];
    IRFunc *ce;
    int k, idx, size;
    if (!opt_unit_f || !c->name || c->target >= 0 || c->sret_addr >= 0 || c->result_addr >= 0 || c->variadic) {
        return NULL;
    }
    idx = opt_unit_find(c->name);
    if (idx < 0 || opt_unit_state[idx] != 2) {
        return NULL;    /* not defined here, or this function's own recursion */
    }
    ce = opt_unit_f[idx];
    if (ce == opt_F || ce->noopt || ce->is_variadic || ce->sret_off || ce->nparams != c->nargs) {
        return NULL;
    }
    size = opt_inst_count(ce);
    if (size > OPT_INLINE_SMALL && !(opt_unit_drop[idx] && opt_unit_calls[idx] == 1 && size <= OPT_INLINE_ONCE)) {
        return NULL;
    }
    if (caller_size + size > OPT_INLINE_GROWTH) {
        return NULL;
    }
    for (k = 0; k < ce->nparams; k++) {
        const IRParam *p = &ce->params[k];
        const IRArg *a = &opt_F->args[c->first_arg + k];
        if (p->vreg < 0 || p->wide || p->by_addr || p->kr_from || a->by_addr || opt_vtype(a->v) != (IRType)ce->vregs[p->vreg].type) {
            return NULL;
        }
    }
    for (k = 0; k < ce->ninsts; k++) {
        const IRInst *i = &ce->insts[k];
        if (i->op == IR_VASTART || i->op == IR_ALLOCA || (i->op == IR_RET && (i->flags & IRF_RECORD))) {
            return NULL;
        }
        if (i->op == IR_RET && call->dst >= 0 && i->a >= 0 && ce->vregs[i->a].type != opt_vtype(call->dst)) {
            return NULL;
        }
    }
    return ce;
}

/* replace the call at position j of block b by a copy of ce's body */
static void opt_inline_call(int b, int j, IRFunc *ce) {
    IRInst call = opt_I[opt_B[b].code[j]];
    int first_arg = opt_F->calls[call.imm].first_arg;
    int vbase = opt_F->nvregs;
    long fbase = opt_F->locals_size;
    int *bmap = xmalloc((size_t)(ce->nblocks + 1) * sizeof(int));
    int k, cb, cont, i;
    IRInst ins;

    for (k = 0; k < ce->nvregs; k++) {
        int v = ir_vreg(opt_F, (IRType)ce->vregs[k].type);
        opt_F->vregs[v].name = ce->vregs[k].name;
        opt_F->vregs[v].is_local = ce->vregs[k].is_local;
    }
    opt_vroom();
    opt_F->locals_size += ce->locals_size;
    /* the callee's blocks, then where its returns go, all placed after b */
    for (cb = 0; cb < ce->nblocks; cb++) {
        bmap[cb] = ce->blocks[cb].start >= 0 && ce->blocks[cb].end > ce->blocks[cb].start ? opt_block_new(b) : -1;
        if (bmap[cb] >= 0) {
            opt_B[bmap[cb]].inlined = 1;
        }
    }
    cont = opt_block_new(b);
    for (k = j + 1; k < opt_B[b].n; k++) {
        opt_push(cont, opt_B[b].code[k]);
    }
    opt_B[b].n = j;
    /* the arguments to the parameters, extended as the prologue would */
    for (k = 0; k < ce->nparams; k++) {
        const IRParam *p = &ce->params[k];
        memset(&ins, 0, sizeof(ins));
        ins.dst = vbase + p->vreg;
        ins.a = opt_F->args[first_arg + k].v;
        ins.b = ins.aux = -1;
        ins.type = (unsigned char)opt_vtype(ins.dst);
        if (!p->is_float && p->size < 4) {
            ins.op = (unsigned char)(p->is_signed ? IR_SEXT : IR_ZEXT);
            ins.size = (unsigned char)p->size;
        } else {
            ins.op = IR_MOV;
        }
        opt_push(b, opt_inst(&ins));
    }
    memset(&ins, 0, sizeof(ins));
    ins.op = IR_BR;
    ins.dst = ins.a = ins.b = ins.aux = -1;
    ins.imm = bmap[0];
    opt_push(b, opt_inst(&ins));

    for (cb = 0; cb < ce->nblocks; cb++) {
        int nb = bmap[cb];
        if (nb < 0) {
            continue;
        }
        for (i = ce->blocks[cb].start; i < ce->blocks[cb].end; i++) {
            ins = ce->insts[i];
            if (ins.op == IR_NOP) {
                continue;
            }
            if (ins.dst >= 0) {
                ins.dst += vbase;
            }
            if (ins.a >= 0) {
                ins.a += vbase;
            }
            if (ins.b >= 0) {
                ins.b += vbase;
            }
            switch (ins.op) {
                case IR_BR:
                    ins.imm = bmap[ins.imm];
                    break;
                case IR_CONDBR:
                    ins.imm = bmap[ins.imm];
                    ins.aux = bmap[ins.aux];
                    break;
                case IR_SWITCH: {
                    const IRSwitch *sw = &ce->switches[ins.imm];
                    int *t = xmalloc((size_t)(sw->n + 1) * sizeof(int));
                    for (k = 0; k < sw->n; k++) {
                        t[k] = bmap[ce->jt[sw->first + k]];
                    }
                    ins.imm = ir_add_switch(opt_F, t, sw->n);
                    ins.aux = bmap[ins.aux];
                    free(t);
                    break;
                }
                case IR_ADDR_LOCAL:
                    ins.imm -= fbase;
                    break;
                case IR_ADDR_GLOBAL:
                    ins.sym = ir_name(opt_F, ins.sym);
                    break;
                case IR_VAARG:
                    if (ins.imm) {
                        ins.imm -= fbase;
                    }
                    ins.aux = ir_add_argtype(opt_F, &ce->argtypes[ins.aux]);
                    break;
                case IR_CALL: {
                    IRCall nc = ce->calls[ins.imm];
                    if (nc.name) {
                        nc.name = ir_name(opt_F, nc.name);
                    }
                    if (nc.target >= 0) {
                        nc.target += vbase;
                    }
                    if (nc.sret_addr >= 0) {
                        nc.sret_addr += vbase;
                    }
                    if (nc.result_addr >= 0) {
                        nc.result_addr += vbase;
                    }
                    nc.first_arg = opt_F->nargs;
                    for (k = 0; k < nc.nargs; k++) {
                        IRArg a = ce->args[ce->calls[ins.imm].first_arg + k];
                        a.v += vbase;
                        ir_add_arg(opt_F, &a);
                    }
                    ins.imm = ir_add_call(opt_F, &nc);
                    break;
                }
                case IR_RET: {
                    /* the result to the call's destination, then on after the call */
                    if (call.dst >= 0) {
                        IRInst r;
                        memset(&r, 0, sizeof(r));
                        r.dst = call.dst;
                        r.b = r.aux = -1;
                        r.type = (unsigned char)opt_vtype(call.dst);
                        if (ins.a >= 0) {
                            r.op = IR_MOV;
                            r.a = ins.a;
                        } else {
                            r.op = (unsigned char)(ir_type_is_float((IRType)r.type) ? IR_FCONST : IR_ICONST);
                            r.a = -1;
                        }
                        opt_push(nb, opt_inst(&r));
                    }
                    memset(&ins, 0, sizeof(ins));
                    ins.op = IR_BR;
                    ins.dst = ins.a = ins.b = ins.aux = -1;
                    ins.imm = cont;
                    break;
                }
                default:
                    break;
            }
            opt_push(nb, opt_inst(&ins));
        }
    }
    free(bmap);
}

/* inline the calls the function makes to small or single-use functions of the unit */
static void opt_inline(void) {
    int b, j, size = 0, any = 0;
    for (b = 0; b < opt_nb; b++) {
        size += opt_B[b].live ? opt_B[b].n : 0;
    }
    /* the blocks made for what follows each inlined call are looked at in their turn */
    for (b = 0; b < opt_nb; b++) {
        if (opt_B[b].inlined) {
            continue;
        }
        for (j = 0; opt_B[b].live && j < opt_B[b].n; j++) {
            IRInst *i = &opt_I[opt_B[b].code[j]];
            IRFunc *ce;
            if (i->op != IR_CALL || !(ce = opt_inlinable(i, size))) {
                continue;
            }
            if (opt_inline_limit >= 0 && opt_ninlined >= opt_inline_limit) {
                continue;
            }
            opt_ninlined++;
            if (opt_trace) {
                Loc none;
                memset(&none, 0, sizeof(none));
                diag_note(none, "inlining call %ld, %s into %s", opt_ninlined, ce->name, opt_F->name);
            }
            size += opt_inst_count(ce);
            opt_inline_call(b, j, ce);
            any = 1;
            break;      /* the rest of the block moved to a new one */
        }
    }
    if (any) {
        opt_prune();
        opt_compute_preds();
    }
}

/* ------------------------------------------------------------------ */

static void opt_free_function(void) {
    int b, k;
    for (b = 0; b < opt_nb; b++) {
        free(opt_B[b].code);
        free(opt_B[b].preds);
        free(opt_B[b].df);
    }
    for (k = 0; k < opt_np; k++) {
        free(opt_P[k].v);
    }
    opt_nb = opt_ni = opt_np = 0;
    opt_F = NULL;
}

void opt_function(IRFunc *f) {
    static int disabled = -1;
    static long limit = -1;
    static long count = 0;
    int round;

    if (disabled < 0) {
        const char *l = getenv("KCC_OPT_LIMIT");
        const char *il = getenv("KCC_INLINE_LIMIT");
        disabled = getenv("KCC_NOOPT") != NULL;
        limit = l ? atol(l) : -1;
        opt_inline_limit = il ? atol(il) : -1;
        opt_trace = getenv("KCC_OPT_TRACE") != NULL;
    }
    if (disabled || (limit >= 0 && count >= limit)) {
        return;
    }
    count++;
    if (opt_trace) {
        Loc none;
        memset(&none, 0, sizeof(none));
        diag_note(none, "optimizing function %ld, %s", count, f->name);
    }
    opt_F = f;
    opt_vroom();
    opt_load();
    if (opt_B[0].npreds > 0) {
        /* a jump back to the entry: the parameters would need a phi with no predecessor to take them from */
        opt_free_function();
        return;
    }
    opt_inline();
    opt_thread_flags();
    opt_dominators();
    opt_build_ssa();
    opt_dominators();
    for (round = 0; round < 6; round++) {
        int changed;
        opt_round = round;
        changed = opt_walk();
        if (opt_cfg_changed) {
            opt_prune();
            opt_dominators();
            opt_cfg_changed = 0;
        }
        changed |= opt_thread();
        if (opt_cfg_changed) {
            opt_prune();
            opt_dominators();
        }
        changed |= opt_dce();
        if (!changed) {
            break;
        }
    }
    opt_index();
    opt_licm();
    opt_widen_loads();
    opt_remat();
    opt_sink();
    opt_leave_ssa();
    opt_cleanup_cfg();
    opt_dce_flat();
    opt_cleanup_cfg();
    opt_renumber();
    opt_store();
    opt_free_function();
}

static int opt_by_name(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    int c = strcmp(opt_unit_f[x]->name, opt_unit_f[y]->name);
    return c ? c : x - y;
}

/* the defined functions f calls directly, as a callback over each */
static int opt_callee(const IRFunc *f, int k) {
    const IRInst *i = &f->insts[k];
    if (i->op != IR_CALL || !f->calls[i->imm].name) {
        return -1;
    }
    return opt_unit_find(f->calls[i->imm].name);
}

void opt_unit(IRFunc **funcs, int n, const char *droppable, char *emit) {
    int *stack = xmalloc((size_t)(2 * n + 2) * sizeof(int));
    int *work = xmalloc((size_t)(n + 1) * sizeof(int));
    char *bad = xcalloc((size_t)n + 1, 1);
    int k, i, sp, nw;

    opt_unit_f = funcs;
    opt_unit_n = n;
    opt_unit_drop = droppable;
    opt_unit_sorted = xmalloc((size_t)(n + 1) * sizeof(int));
    opt_unit_state = xcalloc((size_t)n + 1, sizeof(int));
    opt_unit_calls = xcalloc((size_t)n + 1, sizeof(int));
    for (k = 0; k < n; k++) {
        opt_unit_sorted[k] = k;
    }
    qsort(opt_unit_sorted, (size_t)n, sizeof(int), opt_by_name);
    for (k = 0; k < n; k++) {
        for (i = 0; i < funcs[k]->ninsts; i++) {
            int c = opt_callee(funcs[k], i);
            if (c >= 0) {
                opt_unit_calls[c]++;
            }
        }
    }
    /* callees first: a depth-first walk of the call graph, each function optimized as it is left */
    for (k = 0; k < n; k++) {
        if (opt_unit_state[k]) {
            continue;
        }
        sp = 0;
        stack[sp++] = k;
        stack[sp++] = 0;
        opt_unit_state[k] = 1;
        while (sp) {
            int f = stack[sp - 2];
            int next = stack[sp - 1];
            int c = -1;
            while (next < funcs[f]->ninsts) {
                c = opt_callee(funcs[f], next++);
                if (c >= 0 && opt_unit_state[c] == 0) {
                    break;
                }
                c = -1;
            }
            stack[sp - 1] = next;
            if (c >= 0) {
                opt_unit_state[c] = 1;
                stack[sp++] = c;
                stack[sp++] = 0;
                continue;
            }
            if (!funcs[f]->noopt) {
                opt_function(funcs[f]);
                /* a verifier failure is an internal error, already reported; the function is not selected */
                bad[f] = ir_verify(funcs[f]) != 0;
            }
            opt_unit_state[f] = 2;
            sp -= 2;
        }
    }
    /* what to emit: everything reachable through the calls that remain from what cannot be dropped */
    for (k = 0; k < n; k++) {
        emit[k] = !droppable[k];
    }
    nw = 0;
    for (k = 0; k < n; k++) {
        if (emit[k]) {
            work[nw++] = k;
        }
    }
    while (nw) {
        int f = work[--nw];
        for (i = 0; i < funcs[f]->ninsts; i++) {
            int c = opt_callee(funcs[f], i);
            if (c >= 0 && !emit[c]) {
                emit[c] = 1;
                work[nw++] = c;
            }
        }
    }
    for (k = 0; k < n; k++) {
        if (bad[k]) {
            emit[k] = 0;
        }
    }
    free(bad);
    free(stack);
    free(work);
    free(opt_unit_sorted);
    free(opt_unit_state);
    free(opt_unit_calls);
    opt_unit_f = NULL;
    opt_unit_n = 0;
    opt_unit_sorted = opt_unit_state = opt_unit_calls = NULL;
    opt_unit_drop = NULL;
}
