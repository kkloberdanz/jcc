/*
 * The Lua milestone run in process (Step 15 of the plan): a host that
 * compiles the whole Lua 5.4.8 interpreter as one unit (the sources
 * #included in the order of Lua's own onelua.c, which the release does
 * not ship), loads it into itself with jcc_compile_to_jit, and calls the
 * module's main with the rest of its arguments, so that Lua's test suite
 * runs on jit-compiled code. The interpreter calls the C library (stdio,
 * setjmp/longjmp for Lua errors, math, locale, time, dlopen) through the
 * process. Built and run by tests/run-lua.sh jit and jit-x86_64.
 *
 *     lua-jit [-O level] <lua src dir> <lua arguments...>
 */
/* snprintf, which glibc hides under -std=c89 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jcc.h"

static const char *const sources[] = {
    /* core */
    "lzio.c", "lctype.c", "lopcodes.c", "lmem.c", "lundump.c", "ldump.c", "lstate.c", "lgc.c",
    "llex.c", "lcode.c", "lparser.c", "ldebug.c", "lfunc.c", "lobject.c", "ltm.c", "lstring.c",
    "ltable.c", "ldo.c", "lvm.c", "lapi.c",
    /* auxiliary library */
    "lauxlib.c",
    /* standard libraries */
    "lbaselib.c", "lcorolib.c", "ldblib.c", "liolib.c", "lmathlib.c", "loadlib.c", "loslib.c",
    "lstrlib.c", "ltablib.c", "lutf8lib.c", "linit.c",
    /* the interpreter */
    "lua.c"
};

int main(int argc, char **argv) {
    const char *level = "0";
    const char *dir;
    char unit[4096];
    size_t n = 0;
    size_t i;
    Jcc *k;
    JccJit *j;
    int (*lua_main)(int, char **);
    int status;

    if (argc > 2 && strcmp(argv[1], "-O") == 0) {
        level = argv[2];
        argv += 2;
        argc -= 2;
    }
    if (argc < 2) {
        fprintf(stderr, "usage: lua-jit [-O level] <lua src dir> <lua arguments...>\n");
        return 2;
    }
    dir = argv[1];

    /* onelua.c: luaconf.h set up for the core and the libraries, the internal functions static, lua.c's main last */
    n += (size_t)snprintf(unit + n, sizeof(unit) - n,
                          "#include \"lprefix.h\"\n"
                          "#define LUA_CORE\n"
                          "#define LUA_LIB\n"
                          "#define ltable_c\n"
                          "#define lvm_c\n"
                          "#include \"luaconf.h\"\n"
                          "#undef LUAI_FUNC\n"
                          "#undef LUAI_DDEC\n"
                          "#undef LUAI_DDEF\n"
                          "#define LUAI_FUNC static\n"
                          "#define LUAI_DDEC(def)\n"
                          "#define LUAI_DDEF static\n");
    for (i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        n += (size_t)snprintf(unit + n, sizeof(unit) - n, "#include \"%s\"\n", sources[i]);
    }

    k = jcc_new(NULL);
    jcc_set_option(k, "-I", dir);
    jcc_set_option(k, "-std", "gnu99");
    jcc_set_option(k, "-O", level);
    jcc_set_option(k, "-D", "LUA_COMPAT_5_3");
    jcc_set_option(k, "-D", "LUA_USE_JUMPTABLE=0");     /* no computed goto */
#if defined(__APPLE__)
    jcc_set_option(k, "-D", "LUA_USE_MACOSX");
#else
    jcc_set_option(k, "-D", "LUA_USE_LINUX");
#endif
    j = jcc_compile_to_jit(k, "onelua.c", unit);
    jcc_free(k);
    if (!j) {
        fprintf(stderr, "FAIL: the interpreter did not compile and load\n");
        return 1;
    }
    lua_main = (int (*)(int, char **))jcc_jit_symbol(j, "main");
    if (!lua_main) {
        fprintf(stderr, "FAIL: the module has no main\n");
        jcc_jit_unload(j);
        return 1;
    }
    argv[1] = "lua";
    fflush(stdout);
    status = lua_main(argc - 1, argv + 1);
    fflush(stdout);
    jcc_jit_unload(j);
    return status;
}
