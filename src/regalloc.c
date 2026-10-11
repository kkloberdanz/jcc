/*
 * Register allocation (src/regalloc.h): move coalescing, liveness, one
 * interval per virtual register, linear scan in three tiers,
 * callee-saved bookkeeping and the frame layout. JCC_NOREGALLOC=1 in
 * the environment skips the scan, so every value lives in a slot: when
 * a program works that way and not otherwise, the bug is in here. Positions are the instructions in the order the
 * backends emit them (block order, each block contiguous), so an
 * interval is a pair of positions and two intervals conflict when they
 * share one: a value dying at an instruction and the value it defines
 * never share a register, which lets the x86_64 selectors write the
 * destination before they have read every operand.
 */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "regalloc.h"

typedef unsigned long Word;
#define WBITS 64

/* the register class of a type: 0 integer, 1 floating point, -1 for the values that live in memory */
static int class_of(IRType t) {
    return t == IR_I32 || t == IR_I64 ? 0 : t == IR_F32 || t == IR_F64 ? 1 : -1;
}

static IRFunc *RF = NULL;
static const RegTarget *T = NULL;

/* buffers kept between functions; `cap` is in bytes */
typedef struct Scratch {
    void *p;
    size_t cap;
} Scratch;

static void *grow(Scratch *b, size_t need) {
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < need) {
            cap *= 2;
        }
        b->p = realloc(b->p, cap);
        if (!b->p) {
            fatal("out of memory");
        }
        b->cap = cap;
    }
    return b->p;
}

static Scratch b_pos, b_at, b_blk_of, b_bfirst, b_blast, b_depth, b_live_in, b_live, b_start, b_end, b_weight, b_clob_args,
    b_clob_call, b_uses, b_lowest, b_order, b_skip, b_is_param, b_alias, b_ndefs, b_home;

static int nw;              /* words per bit set */
static int *pos;            /* per instruction: its position, -1 when its block is never placed */
static int *at;             /* per position: the instruction */
static int *blk_of;         /* per instruction: its block */
static int npos;
static int *bfirst;         /* per block: the positions of its first and last instruction, -1 for an unplaced block */
static int *blast;
static int *depth;          /* per block: loop nesting, from the back edges */
static Word *live_in;       /* per block: the registers live on entry */
static Word *live;          /* a scratch set */
static int *start;          /* per register: its interval; start -1 for a parameter, which the prologue defines */
static int *end;
static long *weight;        /* per register: the spill cost, uses weighted by loop depth */
static int *clob_args;      /* per position: the instructions in [0, p] whose code writes the argument tier, or everything caller-saved */
static int *clob_call;
static int *uses;           /* the registers an instruction reads */
static int *lowest;         /* per register: the lowest tier it may take (0 arguments, 1 caller-saved, 2 callee-saved) */
static int *order;          /* the registers with intervals, by start */
static char *skip;          /* per register: needs no place (folded, or a compare fused with its branch) */
static char *is_param;
static int *alias;          /* per register: the register it was merged into (a temporary moved into a local), or itself */
static int *ndefs;          /* per register: the instructions that write it */
static int *home;           /* per register, at -O1: the class index it would rather have (a parameter's argument register, x0 for a returned value), or -1 */

/* ------------------------------------------------------------------ */
/* bit sets                                                           */
/* ------------------------------------------------------------------ */

static void set_bit(Word *s, int v) {
    s[v / WBITS] |= 1ul << (v % WBITS);
}

static void clear_bit(Word *s, int v) {
    s[v / WBITS] &= ~(1ul << (v % WBITS));
}

static int lowest_bit(Word m) {
    int b = 0;
    while ((m & 0xffff) == 0) {
        m >>= 16;
        b += 16;
    }
    while ((m & 1) == 0) {
        m >>= 1;
        b++;
    }
    return b;
}

/* ------------------------------------------------------------------ */
/* the CFG                                                            */
/* ------------------------------------------------------------------ */

static void layout(void) {
    int b, i;
    int p = 0;
    for (i = 0; i < RF->ninsts; i++) {
        pos[i] = -1;
    }
    for (b = 0; b < RF->nblocks; b++) {
        const IRBlock *blk = &RF->blocks[b];
        bfirst[b] = blast[b] = -1;
        if (blk->start < 0 || blk->end <= blk->start) {
            continue;
        }
        bfirst[b] = p;
        for (i = blk->start; i < blk->end; i++) {
            pos[i] = p;
            at[p] = i;
            blk_of[i] = b;
            p++;
        }
        blast[b] = p - 1;
    }
    npos = p;
}

/* the terminator of placed block b, or NULL */
static const IRInst *block_term(int b) {
    return blast[b] < 0 ? NULL : &RF->insts[at[blast[b]]];
}

/* the registers instruction `i` reads, into uses[]; a call's record result address is read after the call returns and is listed last */
static int inst_reads(const IRInst *i, int *nafter) {
    int n = 0;
    *nafter = 0;
    if (i->a >= 0) {
        uses[n++] = i->a;
    }
    if (i->b >= 0) {
        uses[n++] = i->b;
    }
    if (i->op == IR_CALL) {
        const IRCall *c = &RF->calls[i->imm];
        int k;
        if (c->target >= 0) {
            uses[n++] = c->target;
        }
        if (c->sret_addr >= 0) {
            uses[n++] = c->sret_addr;
        }
        for (k = 0; k < c->nargs; k++) {
            uses[n++] = RF->args[c->first_arg + k].v;
        }
        if (c->result_addr >= 0) {
            uses[n++] = c->result_addr;
            *nafter = 1;
        }
    }
    return n;
}

static int find(int v) {
    while (alias[v] != v) {
        v = alias[v];
    }
    return v;
}

/*
 * Move coalescing. Lowering computes an expression into a temporary and
 * moves it to the local it is assigned to; when the temporary is written
 * once, read only by that move, and nothing between its definition and
 * the move reads or writes the local, the two are one register: the
 * operation writes the local directly and the move disappears (the
 * selectors emit nothing for a move between the same register). The
 * definition may itself read the local (`s = s + x`): the x86_64
 * selectors handle a destination that is also an operand.
 */
static void coalesce(void) {
    int v, i, k, j, n, nafter;
    for (v = 0; v < RF->nvregs; v++) {
        alias[v] = v;
        ndefs[v] = 0;
    }
    for (i = 0; i < RF->ninsts; i++) {
        if (RF->insts[i].dst >= 0) {
            ndefs[RF->insts[i].dst]++;
        }
    }
    for (i = 0; i < RF->ninsts; i++) {
        const IRInst *m = &RF->insts[i];
        const IRVReg *tv;
        int t, l, d;
        if (m->op != IR_MOV || m->dst < 0 || m->a < 0 || m->a == m->dst || pos[i] < 0) {
            continue;
        }
        t = m->a;
        l = m->dst;
        tv = &RF->vregs[t];
        d = tv->def;
        if (tv->is_local || tv->nuses != 1 || ndefs[t] != 1 || d < 0 || d >= i || tv->type != RF->vregs[l].type
                || class_of((IRType)tv->type) < 0 || blk_of[d] != blk_of[i]) {
            continue;
        }
        for (k = d + 1; k < i; k++) {
            if (RF->insts[k].dst == l) {
                break;
            }
            n = inst_reads(&RF->insts[k], &nafter);
            for (j = 0; j < n; j++) {
                if (uses[j] == l) {
                    break;
                }
            }
            if (j < n) {
                break;
            }
        }
        if (k < i) {
            continue;
        }
        alias[t] = l;
    }
}

/* `out` = the union of the successors' live-in sets */
static void live_out(int b, Word *out) {
    const IRInst *t = block_term(b);
    int n = t ? ir_nsuccs(RF, t) : 0;
    int k, w;
    memset(out, 0, (size_t)nw * sizeof(Word));
    for (k = 0; k < n; k++) {
        const Word *s = &live_in[(size_t)ir_succ(RF, t, k) * (size_t)nw];
        for (w = 0; w < nw; w++) {
            out[w] |= s[w];
        }
    }
}

/* backward dataflow to a fixed point, blocks in reverse layout order */
static void liveness(void) {
    int changed = 1;
    int b, p, k, n, nafter;
    memset(live_in, 0, (size_t)RF->nblocks * (size_t)nw * sizeof(Word));
    while (changed) {
        changed = 0;
        for (b = RF->nblocks - 1; b >= 0; b--) {
            Word *in;
            if (bfirst[b] < 0) {
                continue;
            }
            live_out(b, live);
            for (p = blast[b]; p >= bfirst[b]; p--) {
                const IRInst *ins = &RF->insts[at[p]];
                if (ins->dst >= 0) {
                    clear_bit(live, find(ins->dst));
                }
                n = inst_reads(ins, &nafter);
                for (k = 0; k < n; k++) {
                    set_bit(live, find(uses[k]));
                }
            }
            in = &live_in[(size_t)b * (size_t)nw];
            if (memcmp(in, live, (size_t)nw * sizeof(Word)) != 0) {
                memcpy(in, live, (size_t)nw * sizeof(Word));
                changed = 1;
            }
        }
    }
}

/* a branch to a block at or before its own is a back edge; every block between is in that loop */
static void loop_depths(void) {
    int b, k, x;
    for (b = 0; b < RF->nblocks; b++) {
        depth[b] = 0;
    }
    for (b = 0; b < RF->nblocks; b++) {
        const IRInst *t = block_term(b);
        int n = t ? ir_nsuccs(RF, t) : 0;
        for (k = 0; k < n; k++) {
            int s = ir_succ(RF, t, k);
            if (bfirst[s] < 0 || bfirst[s] > bfirst[b]) {
                continue;
            }
            for (x = s; x <= b; x++) {
                depth[x]++;
            }
        }
    }
}

static void clobbers(void) {
    int p;
    int na = 0;
    int nc = 0;
    for (p = 0; p < npos; p++) {
        int c = T->clobbers(RF, &RF->insts[at[p]]);
        na += c != RA_CLOBBER_NONE;
        nc += c == RA_CLOBBER_CALL;
        clob_args[p] = na;
        clob_call[p] = nc;
    }
}

/* the clobbers of one kind at positions lo..hi */
static int clobbers_in(const int *prefix, int lo, int hi) {
    if (lo < 0) {
        lo = 0;
    }
    if (hi >= npos) {
        hi = npos - 1;
    }
    if (lo > hi) {
        return 0;
    }
    return prefix[hi] - (lo > 0 ? prefix[lo - 1] : 0);
}

/* ------------------------------------------------------------------ */
/* intervals                                                          */
/* ------------------------------------------------------------------ */

static void extend_to(int v, int p) {
    if (start[v] > p) {
        start[v] = p;
    }
    if (end[v] < p) {
        end[v] = p;
    }
}

static void extend_set(const Word *s, int p) {
    int w;
    for (w = 0; w < nw; w++) {
        Word m = s[w];
        while (m) {
            extend_to(w * WBITS + lowest_bit(m), p);
            m &= m - 1;
        }
    }
}

/* a compare whose only use is the branch right after it: both backends fuse the two and never materialize the result */
static int fused_compare(int i) {
    const IRInst *ins = &RF->insts[i];
    return (ins->op == IR_ICMP || ins->op == IR_FCMP) && ins->dst >= 0 && RF->vregs[ins->dst].nuses == 1
        && i + 1 < RF->ninsts && RF->insts[i + 1].op == IR_CONDBR && RF->insts[i + 1].a == ins->dst;
}

static void intervals(void) {
    static const long scale[4] = { 1, 10, 100, 1000 };
    int v, b, p, k, n, nafter;

    for (v = 0; v < RF->nvregs; v++) {
        start[v] = INT_MAX;
        end[v] = -1;
        weight[v] = 0;
        skip[v] = RF->vregs[v].folded != 0;
        is_param[v] = 0;
    }
    for (p = 0; p < npos; p++) {
        if (fused_compare(at[p])) {
            skip[RF->insts[at[p]].dst] = 1;
        }
    }
    for (b = 0; b < RF->nblocks; b++) {
        long w;
        if (bfirst[b] < 0) {
            continue;
        }
        w = scale[depth[b] > 3 ? 3 : depth[b]];
        extend_set(&live_in[(size_t)b * (size_t)nw], bfirst[b]);
        live_out(b, live);
        extend_set(live, blast[b]);
        for (p = bfirst[b]; p <= blast[b]; p++) {
            const IRInst *ins = &RF->insts[at[p]];
            if (ins->dst >= 0) {
                int d = find(ins->dst);
                extend_to(d, p);
                weight[d] += w;
            }
            n = inst_reads(ins, &nafter);
            for (k = 0; k < n; k++) {
                int u = find(uses[k]);
                /* a folded address's operand is read by the access after it, so must survive what that clobbers */
                int folded = ins->dst >= 0 && RF->vregs[ins->dst].folded && ins->op == IR_ADD && p + 1 < npos
                    && T->clobbers(RF, &RF->insts[at[p + 1]]) != RA_CLOBBER_NONE;
                extend_to(u, k >= n - nafter || folded ? p + 1 : p);
                weight[u] += w;
            }
        }
    }
    for (k = 0; k < RF->nparams; k++) {
        v = RF->params[k].vreg;
        if (v >= 0 && end[v] >= 0) {
            start[v] = -1;
            is_param[v] = 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* linear scan                                                        */
/* ------------------------------------------------------------------ */

/* the index in class c of machine register m among its argument registers, or -1 */
static int arg_index(const RegClass *c, int m) {
    int r;
    for (r = 0; r < c->nargs; r++) {
        if (c->regs[r] == m) {
            return r;
        }
    }
    return -1;
}

/*
 * At -O1 a parameter that nothing between the entry and its last use
 * clobbers stays in the register it arrived in: the prologue moves only
 * into registers outside the argument tier, so leaving one where it is
 * disturbs no other. A value only returned would like the result
 * register, the class's first argument register.
 */
static void homes(void) {
    int v, k, i;
    for (v = 0; v < RF->nvregs; v++) {
        home[v] = -1;
    }
    if (RF->opt_level <= 0) {
        return;
    }
    for (k = 0; k < RF->nparams; k++) {
        const IRParam *p = &RF->params[k];
        const RegClass *c;
        v = p->vreg;
        if (v < 0 || !p->loc.in_regs || p->wide || p->by_addr || p->kr_from || class_of((IRType)RF->vregs[v].type) < 0) {
            continue;
        }
        c = class_of((IRType)RF->vregs[v].type) ? &T->floats : &T->ints;
        home[v] = arg_index(c, p->is_float ? (int)p->loc.freg : (int)p->loc.reg);
    }
    for (i = 0; i < RF->ninsts; i++) {
        const IRInst *ins = &RF->insts[i];
        if (ins->op == IR_RET && !(ins->flags & IRF_RECORD) && ins->a >= 0 && !is_param[find(ins->a)]
                && class_of((IRType)RF->vregs[ins->a].type) >= 0) {
            const RegClass *c = class_of((IRType)RF->vregs[ins->a].type) ? &T->floats : &T->ints;
            if (c->ret >= 0) {
                home[find(ins->a)] = c->ret;
            }
        }
    }
}

static int by_start(const void *pa, const void *pb) {
    int a = *(const int *)pa;
    int b = *(const int *)pb;
    if (start[a] != start[b]) {
        return start[a] < start[b] ? -1 : 1;
    }
    return a < b ? -1 : a > b;
}

static int tier_of(const RegClass *c, int r) {
    return r < c->nargs ? 0 : r < c->ncaller ? 1 : 2;
}

/* spill `a` rather than `b`: the lighter one, then the one that ends later */
static int better_victim(int a, int b) {
    if (weight[a] != weight[b]) {
        return weight[a] < weight[b];
    }
    if (end[a] != end[b]) {
        return end[a] > end[b];
    }
    return a > b;
}

#define MAXREGS 64

static void scan(void) {
    const RegClass *classes[2];
    int active[2][MAXREGS];     /* per class: the registers (virtual) holding a machine register */
    int nactive[2];
    int busy[2][MAXREGS];       /* per class and machine register: the virtual register in it, or -1 */
    int used[2][MAXREGS];
    int cls, k, r, n, i;

    classes[0] = &T->ints;
    classes[1] = &T->floats;
    for (cls = 0; cls < 2; cls++) {
        nactive[cls] = 0;
        for (r = 0; r < MAXREGS; r++) {
            busy[cls][r] = -1;
            used[cls][r] = 0;
        }
    }
    n = 0;
    for (i = 0; i < RF->nvregs; i++) {
        if (end[i] >= 0 && !skip[i] && class_of((IRType)RF->vregs[i].type) >= 0) {
            /* what lies between the definition and the last use; an interval can also begin where the value is
               live into a block placed before the one defining it, and then the instruction there counts too */
            int s = start[i];
            int lo = s >= 0 && RF->insts[at[s]].dst >= 0 && find(RF->insts[at[s]].dst) == i ? s + 1 : s < 0 ? 0 : s;
            order[n++] = i;
            lowest[i] = clobbers_in(clob_call, lo, end[i] - 1) > 0 ? 2
                      : is_param[i] || clobbers_in(clob_args, lo, end[i]) > 0 ? 1 : 0;
        }
    }
    qsort(order, (size_t)n, sizeof(*order), by_start);

    for (i = 0; i < n; i++) {
        int v = order[i];
        const RegClass *c;
        int *act;
        int reg = -1;
        int victim;

        cls = class_of((IRType)RF->vregs[v].type);
        c = classes[cls];
        act = active[cls];
        /* expire what ended before this one starts */
        for (k = 0; k < nactive[cls]; ) {
            if (end[act[k]] < start[v]) {
                busy[cls][RF->vregs[act[k]].reg] = -1;
                act[k] = act[--nactive[cls]];
            } else {
                k++;
            }
        }
        /* its home when it is free and nothing in between clobbers it */
        if (home[v] >= 0 && busy[cls][home[v]] < 0
                && (is_param[v] ? clobbers_in(clob_args, 0, end[v]) == 0 : lowest[v] == 0)) {
            reg = home[v];
        }
        /* a free register of the lowest tier allowed, the argument registers first */
        for (r = lowest[v] == 0 ? 0 : lowest[v] == 1 ? c->nargs : c->ncaller; reg < 0 && r < c->n; r++) {
            if (busy[cls][r] < 0) {
                reg = r;
                break;
            }
        }
        if (reg < 0) {
            /* none: spill the lightest of this one and the active ones in registers it could take */
            victim = v;
            for (k = 0; k < nactive[cls]; k++) {
                if (tier_of(c, RF->vregs[act[k]].reg) >= lowest[v] && better_victim(act[k], victim)) {
                    victim = act[k];
                }
            }
            if (victim == v) {
                continue;
            }
            reg = RF->vregs[victim].reg;
            RF->vregs[victim].reg = -1;
            for (k = 0; k < nactive[cls]; k++) {
                if (act[k] == victim) {
                    act[k] = act[--nactive[cls]];
                    break;
                }
            }
        }
        RF->vregs[v].reg = reg;
        busy[cls][reg] = v;
        used[cls][reg] = 1;
        act[nactive[cls]++] = v;
    }

    RF->nsaved_ints = RF->nsaved_floats = 0;
    for (r = classes[0]->ncaller; r < classes[0]->n; r++) {
        if (used[0][r] && RF->nsaved_ints < 32) {
            RF->saved_ints[RF->nsaved_ints++] = r;
        }
    }
    for (r = classes[1]->ncaller; r < classes[1]->n; r++) {
        if (used[1][r] && RF->nsaved_floats < 32) {
            RF->saved_floats[RF->nsaved_floats++] = r;
        }
    }
}

/* ------------------------------------------------------------------ */
/* the frame                                                          */
/* ------------------------------------------------------------------ */

static void frame(void) {
    long off = -RF->locals_size;
    int v, nsaved;

    for (v = 0; v < RF->nvregs; v++) {
        IRVReg *r = &RF->vregs[v];
        long size;
        if (r->reg >= 0 || skip[v] || alias[v] != v || (!r->is_local && r->nuses == 0)) {
            continue;       /* in a register, merged into another, or never read */
        }
        size = r->type == IR_I128 || r->type == IR_F80 ? 16 : 8;
        off -= size;
        off &= -size;
        r->offset = off;
    }
    nsaved = RF->nsaved_ints + RF->nsaved_floats;
    RF->saved_off = 0;
    if (nsaved) {
        off -= 8 * nsaved;
        off &= -16;
        RF->saved_off = off;
    }
    RF->frame_size = (-off + 15) & ~15L;
    for (v = 0; v < RF->nvregs; v++) {
        if (alias[v] != v) {
            int r = find(v);
            RF->vregs[v].reg = RF->vregs[r].reg;
            RF->vregs[v].offset = RF->vregs[r].offset;
        }
    }
}

/* ------------------------------------------------------------------ */

int regalloc_has_wide(const IRFunc *f, const IRInst *i) {
    int k;
    const int regs[3] = { i->dst, i->a, i->b };
    for (k = 0; k < 3; k++) {
        if (regs[k] >= 0 && (f->vregs[regs[k]].type == IR_I128 || f->vregs[regs[k]].type == IR_F80)) {
            return 1;
        }
    }
    return 0;
}

void regalloc(IRFunc *f, const RegTarget *t) {
    static int bypass = -1;     /* JCC_NOREGALLOC set: every value gets a slot, as in Step 17 */
    int v, k;
    int maxargs = 0;

    if (bypass < 0) {
        bypass = getenv("JCC_NOREGALLOC") != NULL;
    }
    RF = f;
    T = t;
    for (v = 0; v < f->nvregs; v++) {
        f->vregs[v].reg = -1;
        f->vregs[v].offset = 0;
    }
    for (k = 0; k < f->ncalls; k++) {
        if (f->calls[k].nargs > maxargs) {
            maxargs = f->calls[k].nargs;
        }
    }
    nw = (f->nvregs + WBITS - 1) / WBITS;
    pos = grow(&b_pos, (size_t)(f->ninsts + 1) * sizeof(int));
    at = grow(&b_at, (size_t)(f->ninsts + 1) * sizeof(int));
    blk_of = grow(&b_blk_of, (size_t)(f->ninsts + 1) * sizeof(int));
    alias = grow(&b_alias, (size_t)(f->nvregs + 1) * sizeof(int));
    ndefs = grow(&b_ndefs, (size_t)(f->nvregs + 1) * sizeof(int));
    bfirst = grow(&b_bfirst, (size_t)(f->nblocks + 1) * sizeof(int));
    blast = grow(&b_blast, (size_t)(f->nblocks + 1) * sizeof(int));
    depth = grow(&b_depth, (size_t)(f->nblocks + 1) * sizeof(int));
    live_in = grow(&b_live_in, ((size_t)f->nblocks * (size_t)nw + 1) * sizeof(Word));
    live = grow(&b_live, (size_t)(nw + 1) * sizeof(Word));
    start = grow(&b_start, (size_t)(f->nvregs + 1) * sizeof(int));
    end = grow(&b_end, (size_t)(f->nvregs + 1) * sizeof(int));
    weight = grow(&b_weight, (size_t)(f->nvregs + 1) * sizeof(long));
    clob_args = grow(&b_clob_args, (size_t)(f->ninsts + 1) * sizeof(int));
    clob_call = grow(&b_clob_call, (size_t)(f->ninsts + 1) * sizeof(int));
    uses = grow(&b_uses, (size_t)(maxargs + 5) * sizeof(int));
    lowest = grow(&b_lowest, (size_t)(f->nvregs + 1) * sizeof(int));
    order = grow(&b_order, (size_t)(f->nvregs + 1) * sizeof(int));
    skip = grow(&b_skip, (size_t)(f->nvregs + 1));
    is_param = grow(&b_is_param, (size_t)(f->nvregs + 1));
    home = grow(&b_home, (size_t)(f->nvregs + 1) * sizeof(int));

    layout();
    coalesce();
    liveness();
    loop_depths();
    clobbers();
    intervals();
    homes();
    if (bypass) {
        RF->nsaved_ints = RF->nsaved_floats = 0;
    } else {
        scan();
    }
    frame();
    RF = NULL;
    T = NULL;
}
