#ifndef JCC_H
#define JCC_H

#include <stddef.h>

/*
 * jcc as a library. A host program hands it C source it has in memory (or
 * a path) and gets preprocessed text, assembly or an object file back as
 * a buffer; diagnostics arrive through a callback. The command line
 * compiler is a client of this interface.
 *
 * One compile runs at a time: the compiler's state is global, so a host
 * with threads serializes its calls. Everything the library allocates
 * for the host (output buffers) is freed with free().
 *
 * Linking is not part of the library: the driver runs the system linker.
 * Objects are written by jcc's own assembler (Step 14 of the plan), which
 * reads the backends' output and nothing else; -fexternal-as hands the
 * assembly to the host's `cc -c` instead. A unit whose name ends in `.s`
 * is assembly rather than C: it can be checked (jcc_check) or assembled
 * (jcc_compile_to_obj).
 */

typedef struct Jcc Jcc;

typedef enum JccLevel {
    JCC_NOTE,       /* a `note:` following the error it explains */
    JCC_WARNING,
    JCC_ERROR,
    JCC_FATAL       /* the compile was abandoned: out of memory, an internal error */
} JccLevel;

/* `file` is NULL and the numbers 0 for a message without a location; `line` and `col` count from 1 */
typedef void (*JccDiagFn)(void *user, JccLevel level, const char *file, size_t line, size_t col, const char *message);

/*
 * Where source text comes from. The reader is asked first for every file
 * (the main one, if it was given as a path, and every #include by the
 * path the search produced: the including file's directory or the -I
 * directory joined with the name, less a leading "./", so a header next
 * to a unit named "gen.c" is asked for as "gen.h") and answers with the
 * text, which must stay valid until the compile returns, or NULL for a
 * file it does not have, in which case the file system is tried. The
 * include search probes by asking, so a file may be asked for more than
 * once.
 */
typedef const char *(*JccReadFn)(void *user, const char *path, size_t *len);

/* a compiler for `target` (a triple or alias from `jcc --print-targets`; NULL: the host's), or NULL if the target is unknown */
Jcc *jcc_new(const char *target);
void jcc_free(Jcc *k);
const char *jcc_target(const Jcc *k);   /* the canonical name of its target */

/* the diagnostic handler; without one messages are printed to stderr as `file:line:col: error: message` */
void jcc_set_diag(Jcc *k, JccDiagFn fn, void *user);
void jcc_set_reader(Jcc *k, JccReadFn fn, void *user);

/*
 * Options, as the command line spells them: "-I" dir, "-D" NAME or
 * NAME=BODY, "-U" NAME, "-std" c90 or gnu89, "-isysroot" dir, and the
 * flags "-nostdinc", "-ast-dump", "-symtab-dump" (the dumps go to
 * stdout during jcc_check), "-fdump-ir" (each function's IR to stdout
 * as it is compiled) and "-fexternal-as" (objects come from the host's
 * cc rather than jcc's assembler), whose value is ignored; and "-O"
 * with a level: "0" (the default) translates the IR as it is, anything
 * else ("1", "2", "3", "s", "z", or NULL for plain -O) is -O1, which
 * optimizes it. Returns 0, or -1 for an option it does not know.
 */
int jcc_set_option(Jcc *k, const char *name, const char *value);

/*
 * The compiles. `name` is the translation unit's name as diagnostics and
 * __FILE__ report it; `text` is its source, or NULL to read `name` as a
 * path (through the reader, then the file system). Each returns 0 on
 * success and -1 after errors, which went to the diagnostic handler; the
 * output buffer, NUL-terminated beyond `*len`, is the caller's to free().
 */
int jcc_preprocess(Jcc *k, const char *name, const char *text, char **out, size_t *len);
int jcc_check(Jcc *k, const char *name, const char *text);        /* parse and analyse only */
int jcc_compile_to_asm(Jcc *k, const char *name, const char *text, char **out, size_t *len);
int jcc_compile_to_obj(Jcc *k, const char *name, const char *text, char **out, size_t *len);

/* the errors and warnings the last compile reported */
int jcc_errors(const Jcc *k);
int jcc_warnings(const Jcc *k);

#endif /* JCC_H */
