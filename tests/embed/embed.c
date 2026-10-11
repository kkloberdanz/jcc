/*
 * A host program that links libkcc.a: what Goal 2 of the plan is for.
 * It compiles source it holds in memory, serves a header from memory,
 * checks that diagnostics arrive through the callback, that a fatal
 * error leaves the compiler usable, and that an object it got back
 * links and runs. argv[1] is a scratch directory. Built and run by
 * tests/embed.sh (make test).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kcc.h"

static int failures = 0;

static void check(int ok, const char *what) {
    printf("%s embed: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        failures++;
    }
}

/* the diagnostics a compile produced: how many, and the last one */
typedef struct Seen {
    int n;
    KccLevel level;
    char file[128];
    size_t line;
    size_t col;
    char message[256];
} Seen;

static void on_diag(void *user, KccLevel level, const char *file, size_t line, size_t col, const char *message) {
    Seen *s = user;
    s->n++;
    s->level = level;
    snprintf(s->file, sizeof(s->file), "%s", file ? file : "(none)");
    s->line = line;
    s->col = col;
    snprintf(s->message, sizeof(s->message), "%s", message);
}

/* the header the host keeps in memory; the compiler asks for it by the path its search produces */
static const char header_text[] = "#define ANSWER 42\nint helper(int x);\n";

typedef struct Reader {
    int asked;              /* times gen.h was requested */
    char last[256];         /* the last path requested */
} Reader;

static const char *on_read(void *user, const char *path, size_t *len) {
    Reader *r = user;
    snprintf(r->last, sizeof(r->last), "%s", path);
    if (strcmp(path, "gen.h") == 0) {
        r->asked++;
        *len = sizeof(header_text) - 1;
        return header_text;
    }
    return NULL;        /* anything else (stdio.h) comes from the file system */
}

static const char good_text[] =
    "#include <stdio.h>\n"
    "#include \"gen.h\"\n"
    "int helper(int x) { return x + 1; }\n"
    "int main(void) {\n"
    "    printf(\"hello from libkcc %d\\n\", helper(ANSWER - 1));\n"
    "    return ANSWER;\n"
    "}\n";

static int write_file(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    if (fwrite(data, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    return fclose(f);
}

static char *read_file(const char *path) {
    static char buf[256];
    FILE *f = fopen(path, "r");
    size_t n;
    if (!f) {
        return NULL;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    Kcc *k;
    Kcc *cross;
    Seen seen;
    Reader reader;
    char *out = NULL;
    size_t len = 0;
    char path[512];
    char cmd[1200];
    int rc;
    int i;

    /* unknown targets and options are refused, not fatal */
    check(kcc_new("no-such-target") == NULL, "an unknown target gives NULL");
    k = kcc_new(NULL);
    check(k != NULL, "the host target is available");
    if (!k) {
        return 1;
    }
    check(kcc_set_option(k, "-fno-such-option", NULL) == -1, "an unknown option is refused");
    check(kcc_set_option(k, "-std", "gnu89") == 0, "-std is accepted");
    kcc_set_diag(k, on_diag, &seen);
    memset(&reader, 0, sizeof(reader));
    kcc_set_reader(k, on_read, &reader);

    /* 1. an error reaches the handler with its location, and the compile fails */
    memset(&seen, 0, sizeof(seen));
    rc = kcc_compile_to_asm(k, "gen.c", "int main(void) {\n    return nope;\n}\n", &out, &len);
    check(rc == -1 && out == NULL, "a unit with an error fails");
    check(seen.n == 1 && kcc_errors(k) == 1, "exactly one diagnostic was reported");
    check(seen.level == KCC_ERROR, "it is an error");
    check(strcmp(seen.file, "gen.c") == 0 && seen.line == 2 && seen.col == 12, "it has the file, line and column");
    check(strstr(seen.message, "nope") != NULL, "its message names the identifier");

    /* 2. a warning is counted and the compile succeeds */
    memset(&seen, 0, sizeof(seen));
    rc = kcc_compile_to_asm(k, "gen.c", "int;\nint main(void) { return 0; }\n", &out, &len);
    check(rc == 0 && out != NULL && len > 0, "a unit with a warning compiles");
    check(seen.n == 1 && seen.level == KCC_WARNING && kcc_warnings(k) == 1 && kcc_errors(k) == 0, "the warning was reported and counted");
    free(out);

    /* 3. a fatal error is reported as such, and the compiler is usable afterwards */
    memset(&seen, 0, sizeof(seen));
    rc = kcc_compile_to_asm(k, "gen.c", "int main(void) {\n    @\n}\n", &out, &len);
    check(rc == -1 && out == NULL, "a unit with a lexical error fails");
    check(seen.n == 1 && seen.level == KCC_FATAL && seen.line == 2, "it was reported as fatal, with its line");
    memset(&seen, 0, sizeof(seen));
    rc = kcc_compile_to_asm(k, "gen.c", "int main(void) { return 0; }\n", &out, &len);
    check(rc == 0 && seen.n == 0 && out != NULL, "the next unit compiles cleanly after a fatal error");
    free(out);

    /* 4. the preprocessor, with a header served from memory */
    memset(&seen, 0, sizeof(seen));
    rc = kcc_preprocess(k, "gen.c", good_text, &out, &len);
    check(rc == 0 && out != NULL, "preprocessing succeeds");
    check(reader.asked >= 1, reader.asked ? "the header came from the reader" : reader.last);
    check(out && strstr(out, "return 42;") != NULL, "the macro from the in-memory header was expanded");
    check(out && strstr(out, "\"gen.c\"") != NULL, "the output has line markers with the unit's name");
    free(out);

    /* 5. assembly */
    rc = kcc_compile_to_asm(k, "gen.c", good_text, &out, &len);
    check(rc == 0 && out != NULL && strstr(out, "main") != NULL && strlen(out) == len, "assembly comes back as text");
    free(out);

    /* 6. an object that links with cc and runs */
    rc = kcc_compile_to_obj(k, "gen.c", good_text, &out, &len);
    check(rc == 0 && out != NULL && len > 64, "an object comes back");
    snprintf(path, sizeof(path), "%s/gen.o", dir);
    check(out && write_file(path, out, len) == 0, "the object was written");
    free(out);
    snprintf(cmd, sizeof(cmd), "cc -o '%s/gen' '%s/gen.o' && '%s/gen' > '%s/gen.out'", dir, dir, dir, dir);
    rc = system(cmd);
    check(rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 42, "it links, runs and returns the macro's value");
    snprintf(path, sizeof(path), "%s/gen.out", dir);
    check(read_file(path) && strcmp(read_file(path), "hello from libkcc 42\n") == 0, "and prints through the system's libc");

    /* 7. checking only, and a path read through the file system */
    snprintf(path, sizeof(path), "%s/ondisk.c", dir);
    write_file(path, "int f(void) { return 1; }\n", 26);
    check(kcc_check(k, path, NULL) == 0, "a unit named by path is read from the file system");
    check(kcc_check(k, "missing.c", NULL) == -1 && seen.level == KCC_ERROR, "a missing file is an error, not a crash");

    /* 8. the state resets between units: alternate failing and good units */
    rc = 0;
    for (i = 0; i < 20; i++) {
        memset(&seen, 0, sizeof(seen));
        if (kcc_compile_to_asm(k, "gen.c", "int main(void) { return nope; }\n", &out, &len) != -1 || seen.n != 1) {
            rc = 1;
        }
        memset(&seen, 0, sizeof(seen));
        if (kcc_compile_to_asm(k, "gen.c", good_text, &out, &len) != 0 || seen.n != 0 || !out) {
            rc = 1;
        }
        free(out);
    }
    check(rc == 0, "twenty alternating failing and good units behave the same each time");

    /* 9. another target from the same process; -nostdinc so no sysroot is needed */
    cross = kcc_new("x86_64-linux-musl");
    check(cross != NULL && strcmp(kcc_target(cross), "x86_64-linux-musl") == 0, "a second compiler for another target");
    if (cross) {
        kcc_set_option(cross, "-nostdinc", NULL);
        rc = kcc_compile_to_asm(cross, "gen.c", "int main(void) { return 0; }\n", &out, &len);
        check(rc == 0 && out && strstr(out, "GNU-stack") != NULL, "it produces that target's assembly");
        free(out);
        rc = kcc_compile_to_asm(k, "gen.c", "int main(void) { return 0; }\n", &out, &len);
        check(rc == 0 && out && strstr(out, "GNU-stack") == NULL, "and the first still produces the host's");
        free(out);
        kcc_free(cross);
    }

    kcc_free(k);
    printf("EMBED: %d failed\n", failures);
    return failures ? 1 : 0;
}
