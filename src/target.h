#ifndef TARGET_H
#define TARGET_H

#include <stdio.h>

#include "arch.h"
#include "cpp.h"
#include "as/as.h"

/*
 * A target is an architecture and an operating system: the type sizes,
 * the preprocessor's view of the system (predefined macros, header
 * directories) and the code generator's primitives. Every backend is
 * compiled in; --target selects one at run time and the host's is the
 * default.
 */
/*
 * How to link for a target that is not the host's, with ld.lld (the LLVM
 * linker, which knows every ELF target). The link is static: the output
 * needs nothing from the system it runs on. A path starting with "=" is
 * relative to the sysroot. A target that cannot be linked for from
 * another host (Mach-O, which needs ld64) has no TargetLink.
 */
typedef struct TargetLink {
    const char *emulation;          /* ld.lld -m */
    const char *const *startfiles;  /* objects before the program's, NULL-terminated; the static link */
    const char *const *lib_dirs;    /* -L directories */
    const char *const *endfiles;    /* libraries and objects after the program's */
    const char *const *dyn_startfiles;  /* the same for a dynamically linked executable (-dynamic) */
    const char *const *dyn_endfiles;
    const char *dynamic_linker;     /* its loader, as a path on the target */
} TargetLink;

typedef struct Target {
    const char *name;               /* the canonical triple; what cc --target= gets */
    const char *const *aliases;     /* other accepted spellings, NULL-terminated */
    const ArchInfo *info;
    const CppTarget *cpp;
    const Backend *cg;
    const TargetLink *link;         /* NULL: cannot be linked for on another host */
    const struct AsmArch *as;       /* the assembler's syntax and instruction table for it (src/as) */
} Target;

extern const Target target_arm64_macos;
extern const Target target_x86_64_linux;

extern const Target *const targets[];   /* NULL-terminated */
extern const Target *target;            /* the selected one */

/* select by name or alias; NULL selects the host's target. Returns 0 if the name is unknown */
int target_select(const char *name);
const Target *target_host(void);
int target_is_cross(void);              /* the selected target is not the host's */
void target_list(FILE *out);

#endif /* TARGET_H */
