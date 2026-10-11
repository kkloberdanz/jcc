/*
 * The library interface (src/jcc.h): options, the diagnostic and reader
 * hooks, and the pipeline preprocess, parse, sema, codegen that the
 * driver's compile_one used to run itself. A fatal error anywhere below
 * longjmps back to run(), which cleans up what it can and reports failure.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "jcc.h"
#include "cpp.h"
#include "parse.h"
#include "sema.h"
#include "codegen.h"
#include "lower.h"
#include "symtab.h"
#include "type.h"
#include "diag.h"
#include "target.h"
#include "as/as.h"

/* a growable list of owned strings */
typedef struct Strings {
    char **items;
    size_t n;
    size_t cap;
} Strings;

struct Jcc {
    const Target *target;
    JccDiagFn diag;
    void *diag_user;
    JccReadFn read;
    void *read_user;
    char *std;
    char *sysroot;
    Strings include_dirs;
    Strings macros;         /* "-DNAME=BODY" and "-UNAME", in order */
    int nostdinc;
    int ast_dump;
    int symtab_dump;
    int ir_dump;
    int external_as;        /* -fexternal-as: objects from the host's cc, not jcc's assembler */
    int opt_level;          /* -O: 0, or 1 for the optimizing pipeline (src/opt.h) */
    /* the unit being compiled, when its text was given rather than a path */
    const char *main_name;
    const char *main_text;
    size_t main_len;
    int nerrors;
    int nwarnings;
};

static char *copy(const char *s) {
    char *p = malloc(strlen(s) + 1);
    if (!p) {
        fatal("out of memory");
    }
    strcpy(p, s);
    return p;
}

static void strings_add(Strings *l, const char *s) {
    if (l->n == l->cap) {
        char **items;
        l->cap = l->cap ? l->cap * 2 : 8;
        items = realloc(l->items, l->cap * sizeof(*items));
        if (!items) {
            fatal("out of memory");
        }
        l->items = items;
    }
    l->items[l->n++] = copy(s);
}

static void strings_free(Strings *l) {
    size_t i;
    for (i = 0; i < l->n; i++) {
        free(l->items[i]);
    }
    free(l->items);
    l->items = NULL;
    l->n = l->cap = 0;
}

Jcc *jcc_new(const char *name) {
    Jcc *k;
    if (!target_select(name)) {
        return NULL;
    }
    k = calloc(1, sizeof(*k));
    if (!k) {
        return NULL;
    }
    k->target = target;
    return k;
}

void jcc_free(Jcc *k) {
    if (!k) {
        return;
    }
    free(k->std);
    free(k->sysroot);
    strings_free(&k->include_dirs);
    strings_free(&k->macros);
    free(k);
}

const char *jcc_target(const Jcc *k) {
    return k->target->name;
}

void jcc_set_diag(Jcc *k, JccDiagFn fn, void *user) {
    k->diag = fn;
    k->diag_user = user;
}

void jcc_set_reader(Jcc *k, JccReadFn fn, void *user) {
    k->read = fn;
    k->read_user = user;
}

int jcc_set_option(Jcc *k, const char *name, const char *value) {
    jmp_buf recover;
    jmp_buf *previous = diag_set_recovery(&recover);
    int rc = 0;

    if (setjmp(recover)) {
        diag_set_recovery(previous);
        return -1;      /* out of memory, reported */
    }
    if (strcmp(name, "-I") == 0 && value) {
        strings_add(&k->include_dirs, value);
    } else if ((strcmp(name, "-D") == 0 || strcmp(name, "-U") == 0) && value) {
        char *joined = malloc(strlen(name) + strlen(value) + 1);
        if (!joined) {
            fatal("out of memory");
        }
        strcpy(joined, name);
        strcat(joined, value);
        strings_add(&k->macros, joined);
        free(joined);
    } else if (strcmp(name, "-std") == 0 && value) {
        free(k->std);
        k->std = copy(value);
    } else if (strcmp(name, "-isysroot") == 0 && value) {
        free(k->sysroot);
        k->sysroot = copy(value);
    } else if (strcmp(name, "-nostdinc") == 0) {
        k->nostdinc = 1;
    } else if (strcmp(name, "-ast-dump") == 0) {
        k->ast_dump = 1;
    } else if (strcmp(name, "-symtab-dump") == 0) {
        k->symtab_dump = 1;
    } else if (strcmp(name, "-fdump-ir") == 0) {
        k->ir_dump = 1;
    } else if (strcmp(name, "-fexternal-as") == 0) {
        k->external_as = 1;
    } else if (strcmp(name, "-O") == 0) {
        /* 0 is the straight translation; any other level (1, 2, 3, s, z, or none for -O) is -O1 */
        k->opt_level = value && strcmp(value, "0") == 0 ? 0 : 1;
    } else {
        rc = -1;
    }
    diag_set_recovery(previous);
    return rc;
}

int jcc_errors(const Jcc *k) {
    return k->nerrors;
}

int jcc_warnings(const Jcc *k) {
    return k->nwarnings;
}

/* the preprocessor's reader: the main unit's text, then the host's reader, then (in cpp.c) the file system */
static const char *read_source(void *user, const char *path, size_t *len) {
    Jcc *k = user;
    if (k->main_text && strcmp(path, k->main_name) == 0) {
        *len = k->main_len;
        return k->main_text;
    }
    if (k->read) {
        /* the search joins the including file's directory to the name: "./" for a bare unit name is noise to the host */
        if (path[0] == '.' && path[1] == '/') {
            path += 2;
        }
        return k->read(k->read_user, path, len);
    }
    return NULL;
}

/* read a whole stream into a NUL-terminated buffer */
static char *slurp(FILE *f, size_t *len) {
    char *buf = NULL;
    size_t cap = 0, n = 0;
    for (;;) {
        size_t got;
        if (n + 4096 > cap) {
            char *p;
            cap = cap ? cap * 2 : 8192;
            p = realloc(buf, cap);
            if (!p) {
                free(buf);
                fatal("out of memory");
            }
            buf = p;
        }
        got = fread(buf + n, 1, cap - n - 1, f);
        n += got;
        if (got == 0) {
            break;
        }
    }
    buf[n] = '\0';
    *len = n;
    return buf;
}

/* write a buffer to a file; 0 on success */
static int spill(const char *path, const char *text, size_t len) {
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f) {
        return -1;
    }
    ok = fwrite(text, 1, len, f) == len;
    return fclose(f) == 0 && ok ? 0 : -1;
}

/*
 * Assemble `text` with the host's cc, whose integrated assembler knows
 * every target, through a temporary directory; the object comes back as
 * a buffer. The path of -fexternal-as (and of a target jcc's own
 * assembler had no object writer for, of which there are none since
 * Step 14 piece 3).
 */
static int assemble(const char *text, size_t len, char **out, size_t *out_len) {
    char dir[] = "/tmp/jcc.XXXXXX";
    char asm_path[64];
    char obj_path[64];
    char cmd[256];
    FILE *f;
    int rc = -1;

    if (!mkdtemp(dir)) {
        fatal("cannot create a temporary directory");
    }
    snprintf(asm_path, sizeof(asm_path), "%s/out.s", dir);
    snprintf(obj_path, sizeof(obj_path), "%s/out.o", dir);
    if (spill(asm_path, text, len) != 0) {
        fatal("cannot write %s", asm_path);
    }
    if (target_is_cross()) {
        snprintf(cmd, sizeof(cmd), "cc --target=%s -c -o '%s' '%s'", target->name, obj_path, asm_path);
    } else {
        snprintf(cmd, sizeof(cmd), "cc -c -o '%s' '%s'", obj_path, asm_path);
    }
    if (system(cmd) == 0 && (f = fopen(obj_path, "rb")) != NULL) {
        *out = slurp(f, out_len);
        fclose(f);
        rc = 0;
    } else {
        fatal("the assembler failed: %s", cmd);
    }
    unlink(asm_path);
    unlink(obj_path);
    rmdir(dir);
    return rc;
}

typedef enum Stage { PREPROCESS, CHECK, ASSEMBLY, OBJECT } Stage;

/* a unit named `.s` is assembly, not C */
static int is_assembly(const char *name) {
    size_t n = strlen(name);
    return n > 2 && name[n - 2] == '.' && name[n - 1] == 's';
}

/*
 * An assembly unit: its text from the reader or the file system, checked
 * against the assembler's subset, and (for an object) assembled. Only
 * CHECK and OBJECT apply.
 */
static int run_assembly(Jcc *k, const char *name, Stage stage, char **out, size_t *out_len) {
    const char *text;
    size_t len;
    char *owned = NULL;
    int rc = -1;

    if (stage != CHECK && stage != OBJECT) {
        fatal("%s: an assembly file can only be checked or assembled", name);
    }
    text = read_source(k, name, &len);
    if (!text) {
        FILE *f = fopen(name, "rb");
        if (!f) {
            fatal("cannot open %s", name);
        }
        owned = slurp(f, &len);
        fclose(f);
        text = owned;
    }
    if (as_check(name, text, len) == 0) {
        if (stage == CHECK) {
            rc = 0;
        } else if (as_can_write() && !k->external_as) {
            rc = as_assemble(name, text, len, out, out_len);
        } else {
            rc = assemble(text, len, out, out_len);
        }
    }
    free(owned);
    return rc;
}

static int run(Jcc *k, const char *name, const char *text, Stage stage, char **out, size_t *out_len) {
    CppOptions cpp;
    /*
     * Everything assigned after setjmp and read after the longjmp is
     * volatile, or cleanup may see a stale value (asm_text's address is
     * held by the memory stream, which keeps it in memory anyway).
     */
    char *volatile pre = NULL;
    size_t pre_len;
    char *asm_text = NULL;
    size_t asm_len = 0;
    FILE *volatile asm_stream = NULL;
    AST *volatile ast = NULL;
    SymTab *volatile symtab = NULL;
    volatile int status = -1;
    jmp_buf recover;
    jmp_buf *previous;

    if (out) {
        *out = NULL;
        *out_len = 0;
    }
    target_select(k->target->name);
    diag_set_handler(k->diag, k->diag_user);
    diag_reset();
    k->main_name = name;
    k->main_text = text;
    k->main_len = text ? strlen(text) : 0;

    cpp.std = k->std;
    cpp.sysroot = k->sysroot;
    cpp.include_dirs = (const char *const *)k->include_dirs.items;
    cpp.n_include_dirs = k->include_dirs.n;
    cpp.macros = (const char *const *)k->macros.items;
    cpp.n_macros = k->macros.n;
    cpp.nostdinc = k->nostdinc;
    cpp.read = read_source;
    cpp.read_user = k;

    previous = diag_set_recovery(&recover);
    if (setjmp(recover)) {
        goto cleanup;   /* a fatal error, reported already */
    }

    if (is_assembly(name)) {
        status = run_assembly(k, name, stage, out, out_len);
        goto cleanup;
    }

    pre = cpp_text(name, &cpp, &pre_len);
    if (!pre) {
        goto cleanup;
    }
    if (stage == PREPROCESS) {
        *out = pre;
        *out_len = pre_len;
        pre = NULL;
        status = 0;
        goto cleanup;
    }

    symtab = symtab_new();
    ast = parse(pre, symtab);
    free(pre);
    pre = NULL;
    if (!ast || diag_error_count() != 0) {
        /* a syntax error, or an error the parser's actions reported and recovered from */
        goto cleanup;
    }
    if (sema(ast, symtab) != 0) {
        if (k->ast_dump) {
            ast_dump(ast, stdout);
        }
        goto cleanup;
    }
    if (stage == CHECK) {
        if (k->ast_dump) {
            ast_dump(ast, stdout);
        }
        if (k->symtab_dump) {
            symtab_dump(symtab, stdout);
        }
        status = 0;
        goto cleanup;
    }

    asm_stream = open_memstream(&asm_text, &asm_len);
    if (!asm_stream) {
        fatal("out of memory");
    }
    codegen_dump_ir = k->ir_dump;
    codegen_opt_level = k->opt_level;
    if (codegen(ast, symtab, asm_stream) != 0) {
        goto cleanup;
    }
    fclose(asm_stream);
    asm_stream = NULL;
    if (stage == ASSEMBLY) {
        *out = asm_text;
        *out_len = asm_len;
        asm_text = NULL;
        status = 0;
        goto cleanup;
    }
    /*
     * jcc's own assembler makes the object (Step 14); under -fexternal-as
     * the backend's output is checked against its subset and the host's
     * cc makes the object instead.
     */
    if (as_can_write() && !k->external_as) {
        status = as_assemble(name, asm_text, asm_len, out, out_len);
        goto cleanup;
    }
    if (as_check(name, asm_text, asm_len) != 0) {
        goto cleanup;
    }
    status = assemble(asm_text, asm_len, out, out_len);

cleanup:
    diag_set_recovery(previous);
    k->nerrors = diag_error_count();
    k->nwarnings = diag_warning_count();
    k->main_name = NULL;
    k->main_text = NULL;
    free(pre);
    if (asm_stream) {
        fclose(asm_stream);
    }
    free(asm_text);
    parse_cleanup();
    if (ast) {
        ast_free(ast);
    }
    if (symtab) {
        symtab_free(symtab);
    }
    type_free_all();
    diag_set_handler(NULL, NULL);
    return status;
}

int jcc_preprocess(Jcc *k, const char *name, const char *text, char **out, size_t *len) {
    return run(k, name, text, PREPROCESS, out, len);
}

int jcc_check(Jcc *k, const char *name, const char *text) {
    return run(k, name, text, CHECK, NULL, NULL);
}

int jcc_compile_to_asm(Jcc *k, const char *name, const char *text, char **out, size_t *len) {
    return run(k, name, text, ASSEMBLY, out, len);
}

int jcc_compile_to_obj(Jcc *k, const char *name, const char *text, char **out, size_t *len) {
    return run(k, name, text, OBJECT, out, len);
}
