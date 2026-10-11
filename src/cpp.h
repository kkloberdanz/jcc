#ifndef JCC_CPP_H
#define JCC_CPP_H

#include <stddef.h>
#include <stdio.h>

/*
 * The preprocessor: C89 semantics plus the extensions the system headers
 * use (variadic macros, #include_next, #pragma once, __has_include and
 * the other feature queries, _Pragma, GNU line markers). It reads the
 * target's headers from a sysroot and knows the macros the target's
 * compiler predefines, so it does not depend on the host compiler.
 *
 * Output is preprocessed C with `# line "file"` markers, which is exactly
 * what the lexer consumes.
 */

/* a feature query spelled without whitespace, e.g. "__has_feature(nullability)", and its answer */
typedef struct CppHasAnswer {
    const char *query;
    int value;
} CppHasAnswer;

/* what the preprocessor needs to know about a target; each backend defines one */
typedef struct CppTarget {
    const char *default_sysroot;
    const char *const *system_dirs; /* NULL-terminated; "=/usr/include" is relative to the sysroot */
    const char *const *predefined;  /* "NAME BODY" lines, NULL-terminated */
    const char *const *nonstrict;   /* macros defined only outside strict ISO mode */
    const CppHasAnswer *has;        /* answers to feature queries; anything not listed is 0 */
    const char *const *has_builtins; /* the __has_* pseudo macros the target's compiler knows */
} CppTarget;


/*
 * How source text is found. A host that keeps generated code in memory
 * installs a reader: it is asked first for every file (the main one and
 * each #include, by the path the search produced) and answers with the
 * text, which must stay valid until the compile returns, or NULL to say
 * it has no such file, in which case the file system is tried.
 */
typedef const char *(*CppReader)(void *user, const char *path, size_t *len);

typedef struct CppOptions {
    const char *std;                /* NULL: c90 */
    CppReader read;                 /* NULL: the file system only */
    void *read_user;
    const char *sysroot;            /* NULL: the target's default */
    const char *const *include_dirs; /* -I, in order */
    size_t n_include_dirs;
    const char *const *macros;      /* "-DNAME", "-DNAME=BODY" and "-UNAME", in order */
    size_t n_macros;
    int nostdinc;                   /* do not search the target's system directories */
} CppOptions;

/*
 * Preprocess `path`: returns the preprocessed text, with `# line "file"`
 * markers, which the caller frees, or NULL after errors (reported through
 * diag.h).
 */
char *cpp_text(const char *path, const CppOptions *opts, size_t *len);

#endif
