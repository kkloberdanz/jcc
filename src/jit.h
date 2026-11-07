#ifndef JIT_H
#define JIT_H

#include <stddef.h>

#include "jcc.h"

/*
 * In-process execution (Step 15 of the plan): src/jit.c loads a
 * relocatable object into this process and resolves it. The public half
 * (jcc_jit_symbol, jcc_jit_unload) is defined there too; libjcc.c wraps
 * jit_load with the diagnostic handler and the recovery point a fatal
 * error returns to.
 *
 * Errors (a malformed object, a relocation out of range, a symbol
 * neither `resolve` nor the process has) go to diag_error; the result
 * is NULL after any.
 */
JccJit *jit_load(const char *obj, size_t len, JccResolveFn resolve, void *user);

#endif /* JIT_H */
