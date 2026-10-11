#ifndef AS_H
#define AS_H

#include <stddef.h>

/*
 * jcc's assembler (Step 14 of the plan). It reads the subset of assembly
 * the backends emit, one fixed table of mnemonics and operand forms per
 * target (src/as/arm64.c, src/as/x86_64.c), and nothing else: a line
 * outside the subset is an error, because the backends are the only
 * intended writer. The target is the selected one (src/target.c).
 *
 * `name` is what diagnostics report as the file; `text` is `len` bytes of
 * assembly, NUL-terminated.
 */

/* parse only: 0 when every line is in the subset, -1 after reporting the lines that are not */
int as_check(const char *name, const char *text, size_t len);

/* assemble to a relocatable object in memory, the buffer the caller's to free(); 0 or -1 after diagnostics */
int as_assemble(const char *name, const char *text, size_t len, char **out, size_t *out_len);

/* the selected target has an object writer (both have since Step 14 piece 3; kept for one that has none) */
int as_can_write(void);

/* each target's half of the assembler: its operand syntax and instruction table (src/as/asm.h) */
struct AsmArch;
extern const struct AsmArch as_arm64;
extern const struct AsmArch as_x86_64;

#endif /* AS_H */
