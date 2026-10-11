#ifndef PEEP_H
#define PEEP_H

#include <stdio.h>

/*
 * A peephole window over the assembly a backend emits. Lines go through
 * peep_add; the backend's rule function looks at the tail of the window
 * (peep_lines[0 .. peep_n)) and rewrites what it recognises with
 * peep_replace; peep_flush writes the window out. Labels and directives
 * end a window, so rules only ever see straight-line code. Since the IR
 * (Step 17) the selectors fold addresses, fuse compares with branches
 * and fall through to the next block themselves, so the one rule each
 * backend has left is the store of a register to a frame slot followed
 * by a load of that slot.
 */
#define PEEP_MAX 24

extern char *peep_lines[PEEP_MAX];
extern int peep_n;

/* start a unit: lines go to `out`; `rules` returns 1 when it changed the window; `enabled` 0 passes lines through */
void peep_begin(FILE *out, int (*rules)(void), int enabled);
void peep_add(const char *line);
void peep_flush(void);
/* replace lines [from, from+count) with `with` (count_with lines) */
void peep_replace(int from, int count, const char *const *with, int count_with);

#endif /* PEEP_H */
