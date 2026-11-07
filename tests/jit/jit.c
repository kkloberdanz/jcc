/*
 * In-process execution (Step 15 of the plan): a host that compiles C it
 * holds in memory, loads it into itself and calls it. It checks a plain
 * function; a unit that calls the C library (variadic snprintf, found
 * by dlsym), uses writable and read-only data, a table of function
 * pointers and a switch big enough for a jump table at -O1, and reaches
 * a host function and a host variable through the resolver; two modules
 * at once, the second calling the first; the errors (an undefined
 * symbol, a non-object, a compile error) and that the compiler works
 * after them; an object from the host's assembler (-fexternal-as); and
 * that loading and unloading a hundred times does not grow the process
 * (not measured under ASan). Each case runs at -O0 and -O1. Built and run by
 * tests/jit.sh (make test), natively on each host; the units include no
 * headers, so that the Linux run needs no header paths.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jcc.h"

/* AddressSanitizer's shadow memory and quarantine move the mapped size: nothing to measure under it */
#if defined(__SANITIZE_ADDRESS__)
#define ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ASAN 1
#endif
#endif

#if defined(ASAN)
static long mapped_size(void) { return -1; }
#elif defined(__APPLE__)
#include <mach/mach.h>
/* the process's virtual size: an unmapped module that stays mapped shows up here */
static long mapped_size(void) {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) {
        return -1;
    }
    return (long)info.virtual_size;
}
#elif defined(__linux__)
/* the number of mappings */
static long mapped_size(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    long n = 0;
    int c;
    if (!f) {
        return -1;
    }
    while ((c = getc(f)) != EOF) {
        n += c == '\n';
    }
    fclose(f);
    return n;
}
#else
static long mapped_size(void) { return -1; }
#endif

static int failures = 0;
static const char *level = "0";

static void check(int ok, const char *what) {
    printf("%s jit -O%s: %s\n", ok ? "PASS" : "FAIL", level, what);
    if (!ok) {
        failures++;
    }
}

/* the diagnostics of a load: how many, and the last message */
typedef struct Seen {
    int n;
    char message[256];
} Seen;

static void on_diag(void *user, JccLevel lvl, const char *file, size_t line, size_t col, const char *message) {
    Seen *s = user;
    (void)lvl;
    (void)file;
    (void)line;
    (void)col;
    s->n++;
    snprintf(s->message, sizeof(s->message), "%s", message);
}

/* what the host offers a module through the resolver */
int host_counter = 0;
static int host_add(int a, int b) { return a + b + host_counter; }

typedef struct Host {
    int asked;
    const JccJit *other;     /* a module whose symbols are offered too */
} Host;

static void *on_resolve(void *user, const char *name) {
    Host *h = user;
    h->asked++;
    if (strcmp(name, "host_add") == 0) {
        return (void *)host_add;
    }
    if (strcmp(name, "host_counter") == 0) {
        return &host_counter;
    }
    if (h->other) {
        return jcc_jit_symbol(h->other, name);
    }
    return NULL;
}

static const char unit_plain[] = "int twice_plus_one(int x) { return x * 2 + 1; }\n";

static const char unit_rich[] =
    "int snprintf(char *, unsigned long, const char *, ...);\n"
    "int host_add(int, int);\n"
    "extern int host_counter;\n"
    "static const char greeting[] = \"hello\";\n"
    "static int calls;\n"
    "int seeded = 7;\n"
    "static int sq(int x) { return x * x; }\n"
    "static int neg(int x) { return -x; }\n"
    "static int (*const ops[])(int) = { sq, neg };\n"
    "int apply(int which, int x) { calls++; return ops[which](x); }\n"
    "int count(void) { return calls; }\n"
    "int format(char *buf, int n) { return snprintf(buf, 64, \"%s %d %.2f\", greeting, n, n / 4.0); }\n"
    "int pick(int k) {\n"
    "    switch (k) {\n"
    "    case 0: return 10; case 1: return 11; case 2: return 12; case 3: return 13;\n"
    "    case 4: return 14; case 5: return 15; case 6: return 16; case 7: return 17;\n"
    "    default: return -1;\n"
    "    }\n"
    "}\n"
    "int via_host(int a) { host_counter++; return host_add(a, seeded); }\n";

static const char unit_first[] = "int base_value(void) { return 1000; }\n";
static const char unit_second[] =
    "int base_value(void);\n"
    "int derived(int x) { return base_value() + x; }\n";

static void run_cases(void) {
    Jcc *k = jcc_new(NULL);
    Seen seen;
    Host host;
    JccJit *j;
    JccJit *first;
    long before;
    long after;
    int i;
    int ok;

    memset(&seen, 0, sizeof(seen));
    memset(&host, 0, sizeof(host));
    jcc_set_diag(k, on_diag, &seen);
    jcc_set_resolver(k, on_resolve, &host);
    jcc_set_option(k, "-O", level);

    /* a plain function */
    j = jcc_compile_to_jit(k, "plain.c", unit_plain);
    check(j != NULL && seen.n == 0, "a plain unit loads");
    if (j) {
        int (*f)(int) = (int (*)(int))jcc_jit_symbol(j, "twice_plus_one");
        check(f != NULL && f(20) == 41, "its function runs");
        check(jcc_jit_symbol(j, "nothing_by_that_name") == NULL, "an unknown name is NULL");
        jcc_jit_unload(j);
    }

    /* the C library, data, a function table, a jump table, the host's symbols */
    j = jcc_compile_to_jit(k, "rich.c", unit_rich);
    check(j != NULL && seen.n == 0, "a unit using the C library and the host loads");
    if (!j && seen.n) {
        printf("    %s\n", seen.message);
    }
    if (j) {
        int (*apply)(int, int) = (int (*)(int, int))jcc_jit_symbol(j, "apply");
        int (*count)(void) = (int (*)(void))jcc_jit_symbol(j, "count");
        int (*format)(char *, int) = (int (*)(char *, int))jcc_jit_symbol(j, "format");
        int (*pick)(int) = (int (*)(int))jcc_jit_symbol(j, "pick");
        int (*via_host)(int) = (int (*)(int))jcc_jit_symbol(j, "via_host");
        int *seeded = (int *)jcc_jit_symbol(j, "seeded");
        char buf[64];
        check(apply && count && format && pick && via_host && seeded, "every global is found");
        if (apply && count && format && pick && via_host && seeded) {
            check(apply(0, 9) == 81 && apply(1, 9) == -9 && count() == 2, "a function pointer table and a static counter");
            check(format(buf, 10) == 13 && strcmp(buf, "hello 10 2.50") == 0, "snprintf from the C library");
            ok = 1;
            for (i = 0; i < 8; i++) {
                ok &= pick(i) == 10 + i;
            }
            check(ok && pick(8) == -1 && pick(-3) == -1, "a switch");
            check(*seeded == 7, "an initialized global reads from the host");
            *seeded = 30;
            host_counter = 5;
            check(via_host(1) == 1 + 30 + 6 && host_counter == 6, "the host's function and variable, a write from the host");
            check(host.asked >= 2, "the resolver was asked");
        }
        jcc_jit_unload(j);
    }

    /* two modules, the second resolved against the first */
    first = jcc_compile_to_jit(k, "first.c", unit_first);
    host.other = first;
    j = first ? jcc_compile_to_jit(k, "second.c", unit_second) : NULL;
    check(first != NULL && j != NULL, "two modules load at once");
    if (j) {
        int (*derived)(int) = (int (*)(int))jcc_jit_symbol(j, "derived");
        check(derived != NULL && derived(5) == 1005, "one calls the other");
    }
    jcc_jit_unload(j);
    jcc_jit_unload(first);
    host.other = NULL;

    /* errors, and the compiler usable after each */
    seen.n = 0;
    j = jcc_compile_to_jit(k, "undef.c", "int nosuch_function(void);\nint f(void) { return nosuch_function(); }\n");
    check(j == NULL && seen.n == 1 && strstr(seen.message, "nosuch_function") != NULL && jcc_errors(k) == 1,
          "an undefined symbol is reported by name");
    seen.n = 0;
    j = jcc_jit_load(k, "not an object at all", 20);
    check(j == NULL && seen.n == 1 && jcc_errors(k) == 1, "a buffer that is no object is refused");
    seen.n = 0;
    j = jcc_compile_to_jit(k, "bad.c", "int f(void) { return undeclared; }\n");
    check(j == NULL && seen.n >= 1 && jcc_errors(k) >= 1, "a compile error loads nothing");
    seen.n = 0;
    j = jcc_compile_to_jit(k, "plain.c", unit_plain);
    check(j != NULL && seen.n == 0 && ((int (*)(int))jcc_jit_symbol(j, "twice_plus_one"))(1) == 3, "it works after the errors");
    jcc_jit_unload(j);

    /* an object made separately, loaded later and after the compiler is gone */
    {
        char *obj;
        size_t len;
        if (jcc_compile_to_obj(k, "plain.c", unit_plain, &obj, &len) == 0) {
            Jcc *loader = jcc_new(NULL);
            jcc_free(k);
            k = NULL;
            j = jcc_jit_load(loader, obj, len);
            free(obj);
            jcc_free(loader);
            check(j != NULL && ((int (*)(int))jcc_jit_symbol(j, "twice_plus_one"))(4) == 9,
                  "an object from jcc_compile_to_obj, loaded by another Jcc that is freed before the call");
            jcc_jit_unload(j);
        } else {
            check(0, "jcc_compile_to_obj");
        }
    }

    /* an object the host's cc assembled (-fexternal-as): clang's carries __compact_unwind, gcc's .eh_frame */
    k = jcc_new(NULL);
    jcc_set_option(k, "-O", level);
    jcc_set_option(k, "-fexternal-as", NULL);
    jcc_set_resolver(k, on_resolve, &host);
    j = jcc_compile_to_jit(k, "rich.c", unit_rich);
    check(j != NULL, "an object from the host's assembler loads");
    if (j) {
        int (*apply)(int, int) = (int (*)(int, int))jcc_jit_symbol(j, "apply");
        int (*format)(char *, int) = (int (*)(char *, int))jcc_jit_symbol(j, "format");
        int (*pick)(int) = (int (*)(int))jcc_jit_symbol(j, "pick");
        char buf[64];
        check(apply && format && pick && apply(0, 7) == 49 && pick(5) == 15
              && format(buf, 2) == 12 && strcmp(buf, "hello 2 0.50") == 0, "and runs");
        jcc_jit_unload(j);
    }
    jcc_free(k);

    /* a hundred loads and unloads leave nothing mapped; the first rounds warm up malloc */
    k = jcc_new(NULL);
    jcc_set_option(k, "-O", level);
    jcc_set_resolver(k, on_resolve, &host);
    ok = 1;
    before = 0;
    for (i = 0; i < 120; i++) {
        int (*f)(int);
        if (i == 20) {
            before = mapped_size();
        }
        j = jcc_compile_to_jit(k, "rich.c", unit_rich);
        f = j ? (int (*)(int))jcc_jit_symbol(j, "pick") : NULL;
        ok &= f != NULL && f(3) == 13;
        jcc_jit_unload(j);
    }
    after = mapped_size();
    check(ok, "a hundred compiles, loads, calls and unloads");
    if (before < 0 || after < 0) {
        printf("SKIP jit -O%s: no mapping statistics on this platform or under ASan\n", level);
    } else {
        /* a leaked module is at least three pages: 100 of them would be hundreds of kilobytes, or 300 mappings */
#if defined(__APPLE__)
        check(after - before < 256 * 1024, "the mapped size does not grow");
#else
        check(after - before < 50, "the mapping count does not grow");
#endif
        if (after - before >= 256 * 1024 || (after - before >= 50 && after - before < 1024)) {
            printf("    before %ld, after %ld\n", before, after);
        }
    }
    jcc_free(k);
}

int main(void) {
    level = "0";
    run_cases();
    level = "1";
    run_cases();
    if (failures) {
        printf("JIT: %d failed\n", failures);
        return 1;
    }
    printf("JIT OK\n");
    return 0;
}
