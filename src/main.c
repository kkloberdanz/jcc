#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "jcc.h"
#include "target.h"     /* --target and the link tables; the compile itself goes through jcc.h */

struct Options {
    char dont_link;
    char assembly_output;
    char preprocess_only;
    char ast_dump;
    char symtab_dump;
    char ir_dump;
    char external_as;
    char opt_level;     /* -O: '0' (the default) or '1' */
    char syntax_only;
    char *outfile;      /* NULL: a.out, or the input's name with .o or .s */
    const char *infiles[256];       /* .c files to compile and .o/.a files to link, in order */
    size_t n_infiles;
    const char *libs[64];           /* -lNAME, passed to the linker */
    size_t n_libs;
    const char *std;    /* the -std= value, or NULL for the preprocessor's default */
    const char *sysroot; /* -isysroot, or NULL for the target's default */
    const char *target; /* --target, or NULL for the host's */
    int dynamic;        /* -dynamic: a cross link uses the target's shared libc rather than the static one */
    const char *include_dirs[64];   /* -I, in order */
    size_t n_include_dirs;
    const char *macros[256];        /* -D and -U, in order, as given */
    size_t n_macros;
    int nostdinc;
};

static const char *prg_name = "jcc";

/* a message from the driver itself, as opposed to a diagnostic about the source */
static void err_msg(const char *msg) {
    fprintf(stderr, "%s: %s", prg_name, msg);
}

static char *xstrdup(const char *s) {
    char *p = malloc(strlen(s) + 1);
    if (!p) {
        err_msg("out of memory\n");
        exit(EXIT_FAILURE);
    }
    strcpy(p, s);
    return p;
}

/* one line each: a C89 compiler need only accept 509 characters per string literal */
static const char *const help_text[] = {
    "        -c    Compile to object files, don't invoke the linker",
    "",
    "        -dynamic  Link a cross target against its shared libc (the default is static); -static restores it",
    "",
    "        -E    Preprocess only; write the result to stdout (or -o outfile)",
    "",
    "        -h    Display this help menu, then exit",
    "",
    "        -o outfile",
    "              Write the binary (or object, or assembly) to outfile",
    "",
    "        -S    Output assembly and stop",
    "",
    "        -ast-dump",
    "              Print the syntax tree to stdout and stop",
    "",
    "        -symtab-dump",
    "              Print the scopes and declared names to stdout and stop",
    "",
    "        -fdump-ir",
    "              Print each function's IR to stdout as it is compiled",
    "",
    "        -fexternal-as",
    "              Assemble with the host's cc instead of jcc's own assembler",
    "",
    "        -fsyntax-only",
    "              Stop after parsing and semantic analysis",
    "",
    "        -O0, -O1, -O, -O2, -O3, -Os, -Oz",
    "              Optimization level: -O0 (the default) generates code straight from the IR;",
    "              -O1 optimizes it, and -O, -O2, -O3, -Os and -Oz are the same as -O1",
    "",
    "        -lNAME",
    "              Link with library NAME; .o and .a arguments are linked as given",
    "",
    "        -Idir, -Dmacro[=value], -Umacro",
    "              Add an include directory, define or undefine a macro",
    "",
    "        -std=standard",
    "              c90 (the default, strict ISO) or gnu89",
    "",
    "        -isysroot dir, -nostdinc",
    "              Where the target's system headers are; or search none",
    "",
    "        --target=triple, --print-targets",
    "              Compile for another target (the host's is the default); or list them",
    "",
};

static void print_help(void) {
    size_t i;
    printf("\njcc -- an embeddable C compiler\n");
    printf("\n%s: [-c] [-E] [-h] [-S] [-ast-dump] [-symtab-dump] [-fdump-ir] [-fexternal-as] [-fsyntax-only] [-Olevel] [--target=triple] [-o outfile] [-lNAME] file...\n\n", prg_name);
    for (i = 0; i < sizeof(help_text) / sizeof(help_text[0]); i++) {
        printf("%s\n", help_text[i]);
    }
}

/*
 * Multi-letter single-dash flags (gcc style) are picked out before getopt
 * sees the arguments. Matching entries are removed from argv.
 */
static void parse_long_options(int *argc, char **argv, struct Options *options) {
    int i;
    int n = 1;

    for (i = 1; i < *argc; i++) {
        if (strcmp(argv[i], "-ast-dump") == 0) {
            options->ast_dump = 1;
        } else if (strcmp(argv[i], "-symtab-dump") == 0) {
            options->symtab_dump = 1;
        } else if (strcmp(argv[i], "-fdump-ir") == 0) {
            options->ir_dump = 1;
        } else if (strcmp(argv[i], "-fexternal-as") == 0) {
            options->external_as = 1;
        } else if (strcmp(argv[i], "-fsyntax-only") == 0) {
            options->syntax_only = 1;
        } else if (argv[i][0] == '-' && argv[i][1] == 'O' && strlen(argv[i]) <= 3 && strchr("0123sz", argv[i][2] ? argv[i][2] : '0')) {
            /* -O0 is the default; every other level is -O1, the one optimizing pipeline */
            options->opt_level = argv[i][2] == '0' ? '0' : '1';
        } else if (strncmp(argv[i], "-std=", 5) == 0) {
            options->std = argv[i] + 5;
        } else if (strncmp(argv[i], "--target=", 9) == 0) {
            options->target = argv[i] + 9;
        } else if ((strcmp(argv[i], "--target") == 0 || strcmp(argv[i], "-target") == 0) && i + 1 < *argc) {
            options->target = argv[++i];
        } else if (strcmp(argv[i], "--print-targets") == 0) {
            target_list(stdout);
            exit(EXIT_SUCCESS);
        } else if (strcmp(argv[i], "-dynamic") == 0) {
            options->dynamic = 1;
        } else if (strcmp(argv[i], "-static") == 0) {
            options->dynamic = 0;
        } else if (strcmp(argv[i], "-nostdinc") == 0) {
            options->nostdinc = 1;
        } else if (strcmp(argv[i], "-isysroot") == 0 && i + 1 < *argc) {
            options->sysroot = argv[++i];
        } else if (strncmp(argv[i], "-I", 2) == 0) {
            const char *dir = argv[i][2] ? argv[i] + 2 : (i + 1 < *argc ? argv[++i] : "");
            if (options->n_include_dirs == sizeof(options->include_dirs) / sizeof(options->include_dirs[0])) {
                fprintf(stderr, "%s: too many -I options\n", prg_name);
                exit(EXIT_FAILURE);
            }
            options->include_dirs[options->n_include_dirs++] = dir;
        } else if (strncmp(argv[i], "-l", 2) == 0 && argv[i][2]) {
            if (options->n_libs == sizeof(options->libs) / sizeof(options->libs[0])) {
                fprintf(stderr, "%s: too many -l options\n", prg_name);
                exit(EXIT_FAILURE);
            }
            options->libs[options->n_libs++] = argv[i];
        } else if (strncmp(argv[i], "-D", 2) == 0 || strncmp(argv[i], "-U", 2) == 0) {
            /* `-D NAME` with a separate argument is joined back into one string */
            static char joined[256][256];
            const char *arg = argv[i];
            if (!argv[i][2] && i + 1 < *argc) {
                snprintf(joined[options->n_macros], sizeof(joined[0]), "%s%s", argv[i], argv[i + 1]);
                arg = joined[options->n_macros];
                i++;
            }
            if (options->n_macros == sizeof(options->macros) / sizeof(options->macros[0])) {
                fprintf(stderr, "%s: too many -D and -U options\n", prg_name);
                exit(EXIT_FAILURE);
            }
            options->macros[options->n_macros++] = arg;
        } else {
            argv[n++] = argv[i];
        }
    }
    argv[n] = NULL;
    *argc = n;
}

static void parse_options(int argc, char **argv, struct Options *options) {
    int c;
    options->dont_link = 0;
    options->assembly_output = 0;
    options->preprocess_only = 0;
    options->ast_dump = 0;
    options->symtab_dump = 0;
    options->syntax_only = 0;
    options->n_infiles = 0;
    options->n_libs = 0;
    options->outfile = NULL;
    options->std = NULL;
    options->sysroot = NULL;
    options->target = NULL;
    options->n_include_dirs = 0;
    options->n_macros = 0;
    options->nostdinc = 0;
    options->opt_level = '0';

    parse_long_options(&argc, argv, options);
    if (options->target && strcmp(options->target, "help") == 0) {
        target_list(stdout);
        exit(EXIT_SUCCESS);
    }
    if (!target_select(options->target)) {
        fprintf(stderr, "%s: unknown target '%s'; the targets are:\n", prg_name, options->target);
        target_list(stderr);
        exit(EXIT_FAILURE);
    }

    while ((c = getopt(argc, argv, "hESco:")) != -1) {
        switch (c) {
            case 'o':
                options->outfile = optarg;
                break;

            case 'h':
                print_help();
                exit(EXIT_SUCCESS);

            case 'S':
                options->assembly_output = 1;
                break;

            case 'E':
                options->preprocess_only = 1;
                break;

            case 'c':
                options->dont_link = 1;
                break;

            case '?':
                break;

            default:
              print_help();
              exit(EXIT_FAILURE);
        }
    }

    for (; optind < argc; optind++) {
        if (options->n_infiles == sizeof(options->infiles) / sizeof(options->infiles[0])) {
            fprintf(stderr, "%s: too many input files\n", prg_name);
            exit(EXIT_FAILURE);
        }
        options->infiles[options->n_infiles++] = argv[optind];
    }
    if (options->n_infiles == 0) {
        fprintf(stderr, "%s: no input files\n", prg_name);
        exit(EXIT_FAILURE);
    }
    if (options->outfile && options->n_infiles > 1
            && (options->dont_link || options->assembly_output || options->preprocess_only)) {
        fprintf(stderr, "%s: cannot specify -o with -c, -S or -E and multiple files\n", prg_name);
        exit(EXIT_FAILURE);
    }
}

static const char *default_output(const char *infile, const char *ext) {
    static char buf[1024];
    const char *base = strrchr(infile, '/');
    const char *dot;
    size_t n;

    base = base ? base + 1 : infile;
    dot = strrchr(base, '.');
    n = dot ? (size_t)(dot - base) : strlen(base);
    if (n + strlen(ext) >= sizeof(buf)) {
        n = sizeof(buf) - strlen(ext) - 1;
    }
    memcpy(buf, base, n);
    strcpy(buf + n, ext);
    return buf;
}

/* run the host toolchain for the steps jcc does not do itself yet */
/* append `s` to the command in `buf`, quoted, with "=/dir" made relative to the sysroot */
static void add_arg(char *buf, size_t size, const char *s, const char *sysroot) {
    size_t n = strlen(buf);
    size_t sn = strlen(sysroot);
    while (sn > 1 && sysroot[sn - 1] == '/') {
        sn--;
    }
    if (s[0] == '=') {
        snprintf(buf + n, size - n, " '%.*s%s'", (int)sn, sysroot, s + 1);
    } else if (strncmp(s, "-L=", 3) == 0) {
        snprintf(buf + n, size - n, " '-L%.*s%s'", (int)sn, sysroot, s + 3);
    } else {
        snprintf(buf + n, size - n, " '%s'", s);
    }
}

/*
 * Link for another target with ld.lld, statically, from the sysroot's C
 * runtime and libraries, as the target's own compiler driver would.
 */
static int link_cross(const char *out, const char *const *objs, size_t nobjs, const char *const *libs, size_t nlibs,
                      const char *sysroot, int dynamic) {
    static char cmd[16384];
    const TargetLink *l = target->link;
    const char *const *p;
    char ldir[1024];
    size_t i;
    int rc;

    if (!l) {
        fprintf(stderr, "%s: cannot link for %s on this host; use -c and link on the target\n", prg_name, target->name);
        return -1;
    }
    if (system("command -v ld.lld >/dev/null 2>&1") != 0) {
        fprintf(stderr, "%s: linking for %s needs ld.lld (brew install lld); or use -c and link on the target\n",
                prg_name, target->name);
        return -1;
    }
    if (dynamic) {
        snprintf(cmd, sizeof(cmd), "ld.lld -m %s -dynamic-linker %s -o '%s'", l->emulation, l->dynamic_linker, out);
    } else {
        snprintf(cmd, sizeof(cmd), "ld.lld -m %s -static -o '%s'", l->emulation, out);
    }
    for (p = dynamic ? l->dyn_startfiles : l->startfiles; *p; p++) {
        add_arg(cmd, sizeof(cmd), *p, sysroot);
    }
    for (i = 0; i < nobjs; i++) {
        add_arg(cmd, sizeof(cmd), objs[i], sysroot);
    }
    for (p = l->lib_dirs; *p; p++) {
        snprintf(ldir, sizeof(ldir), "-L%s", *p);
        add_arg(cmd, sizeof(cmd), ldir, sysroot);
    }
    for (i = 0; i < nlibs; i++) {
        add_arg(cmd, sizeof(cmd), libs[i], sysroot);
    }
    for (p = dynamic ? l->dyn_endfiles : l->endfiles; *p; p++) {
        add_arg(cmd, sizeof(cmd), *p, sysroot);
    }
    rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "%s: command failed: %s\n", prg_name, cmd);
        return -1;
    }
    return 0;
}

/* the host toolchain links a native build */
static int link_native(const char *out, const char *const *objs, size_t nobjs, const char *const *libs, size_t nlibs) {
    static char cmd[16384];
    size_t i;
    int rc;

    snprintf(cmd, sizeof(cmd), "cc -o '%s'", out);
    for (i = 0; i < nobjs; i++) {
        add_arg(cmd, sizeof(cmd), objs[i], "/");
    }
    for (i = 0; i < nlibs; i++) {
        add_arg(cmd, sizeof(cmd), libs[i], "/");
    }
    rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "%s: command failed: %s\n", prg_name, cmd);
        return -1;
    }
    return 0;
}

/* whether `path` is something to compile rather than to hand to the linker */
static int is_source(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot && (strcmp(dot, ".c") == 0 || strcmp(dot, ".i") == 0 || strcmp(dot, ".s") == 0 || strcmp(dot, ".S") == 0) ? 1 : 0;
}

/* write a buffer to `path`, or to stdout when it is NULL; 0 on success */
static int write_output(const char *path, const char *data, size_t len) {
    FILE *f = path ? fopen(path, "wb") : stdout;
    int ok;
    if (!f) {
        err_msg("cannot open the output file\n");
        return -1;
    }
    ok = fwrite(data, 1, len, f) == len;
    if (path) {
        ok = fclose(f) == 0 && ok;
    } else {
        ok = fflush(f) == 0 && ok;
    }
    if (!ok) {
        err_msg("cannot write the output file\n");
        return -1;
    }
    return 0;
}

/*
 * One translation unit, through the library: preprocess, check, assembly
 * or the object `obj_out`, as the options say. Returns 0 on success.
 */
static int compile_one(const struct Options *options, Jcc *k, const char *infile, const char *obj_out) {
    char *out = NULL;
    size_t len = 0;
    int rc;

    if (options->preprocess_only) {
        rc = jcc_preprocess(k, infile, NULL, &out, &len);
        if (rc == 0) {
            rc = write_output(options->outfile, out, len);
        }
    } else if (options->ast_dump || options->symtab_dump || options->syntax_only) {
        rc = jcc_check(k, infile, NULL);
    } else if (options->assembly_output) {
        rc = jcc_compile_to_asm(k, infile, NULL, &out, &len);
        if (rc == 0) {
            rc = write_output(options->outfile ? options->outfile : default_output(infile, ".s"), out, len);
        }
    } else {
        rc = jcc_compile_to_obj(k, infile, NULL, &out, &len);
        if (rc == 0) {
            rc = write_output(obj_out, out, len);
        }
    }
    free(out);
    return rc;
}

/* the compiler the options describe */
static Jcc *make_compiler(const struct Options *options) {
    Jcc *k = jcc_new(options->target);
    size_t i;
    if (!k) {
        err_msg("out of memory\n");
        exit(EXIT_FAILURE);
    }
    for (i = 0; i < options->n_include_dirs; i++) {
        jcc_set_option(k, "-I", options->include_dirs[i]);
    }
    for (i = 0; i < options->n_macros; i++) {
        char name[3];
        name[0] = options->macros[i][0];
        name[1] = options->macros[i][1];
        name[2] = '\0';
        jcc_set_option(k, name, options->macros[i] + 2);
    }
    if (options->std) {
        jcc_set_option(k, "-std", options->std);
    }
    if (options->sysroot) {
        jcc_set_option(k, "-isysroot", options->sysroot);
    }
    if (options->nostdinc) {
        jcc_set_option(k, "-nostdinc", NULL);
    }
    if (options->ast_dump) {
        jcc_set_option(k, "-ast-dump", NULL);
    }
    if (options->symtab_dump) {
        jcc_set_option(k, "-symtab-dump", NULL);
    }
    if (options->external_as) {
        jcc_set_option(k, "-fexternal-as", NULL);
    }
    if (options->ir_dump) {
        jcc_set_option(k, "-fdump-ir", NULL);
    }
    if (options->opt_level != '0') {
        char level[2];
        level[0] = options->opt_level;
        level[1] = '\0';
        jcc_set_option(k, "-O", level);
    }
    return k;
}

int main(int argc, char **argv) {
    struct Options options;
    Jcc *k;
    int status = 1;
    char tmpdir[] = "/tmp/jcc.XXXXXX";
    const char *objs[256];      /* what the linker gets, in order */
    char *temps[256];           /* objects compiled into the temporary directory */
    size_t nobjs = 0;
    size_t ntemps = 0;
    size_t i;

    parse_options(argc, argv, &options);

    k = make_compiler(&options);

    if (!options.dont_link && !options.assembly_output && !options.preprocess_only
            && !options.ast_dump && !options.symtab_dump && !options.syntax_only) {
        if (target_is_cross() && !target->link) {
            fprintf(stderr, "%s: cannot link for %s on this host; use -c and link on the target\n", prg_name, target->name);
            goto cleanup;
        }
        if (!mkdtemp(tmpdir)) {
            err_msg("cannot create a temporary directory\n");
            goto cleanup;
        }
    }

    for (i = 0; i < options.n_infiles; i++) {
        const char *infile = options.infiles[i];
        const char *obj;
        if (!is_source(infile)) {
            objs[nobjs++] = infile;     /* an object or archive: straight to the linker */
            continue;
        }
        if (options.dont_link) {
            obj = options.outfile ? options.outfile : xstrdup(default_output(infile, ".o"));
        } else if (tmpdir[0] == '/' && strcmp(tmpdir, "/tmp/jcc.XXXXXX") != 0) {
            char buf[1200];
            snprintf(buf, sizeof(buf), "%s/%lu-%s", tmpdir, (unsigned long)i, default_output(infile, ".o"));
            obj = temps[ntemps++] = xstrdup(buf);
        } else {
            obj = NULL;     /* -E, -S and the dumps produce no object */
        }
        if (compile_one(&options, k, infile, obj) != 0) {
            goto cleanup;
        }
        if (obj && !options.dont_link) {
            objs[nobjs++] = obj;
        }
    }

    if (!options.dont_link && !options.assembly_output && !options.preprocess_only
            && !options.ast_dump && !options.symtab_dump && !options.syntax_only) {
        const char *exe = options.outfile ? options.outfile : "a.out";
        if (nobjs == 0) {
            fprintf(stderr, "%s: nothing to link\n", prg_name);
            goto cleanup;
        }
        if (target_is_cross()) {
            if (link_cross(exe, objs, nobjs, options.libs, options.n_libs,
                           options.sysroot ? options.sysroot : target->cpp->default_sysroot, options.dynamic) != 0) {
                goto cleanup;
            }
        } else if (link_native(exe, objs, nobjs, options.libs, options.n_libs) != 0) {
            goto cleanup;
        }
    }
    status = 0;

cleanup:
    jcc_free(k);
    for (i = 0; i < ntemps; i++) {
        unlink(temps[i]);
        free(temps[i]);
    }
    if (strcmp(tmpdir, "/tmp/jcc.XXXXXX") != 0) {
        rmdir(tmpdir);
    }
    return status;
}
