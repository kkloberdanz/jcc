CC=cc
CFLAGS=-Werror -Wall -Wextra -std=gnu89 $(OPT) -fPIC
LFLAGS=
HOST_OS=$(shell uname -s)

# Every backend is compiled in; --target selects one at run time and the
# host's is the default. Each target's default header locations are baked
# in here: the sysroot and the compiler-provided headers (stddef.h,
# stdarg.h, ...). A "=/dir" entry is relative to the sysroot, so -isysroot
# moves it; -isysroot and -I override everything at run time.
ifeq ($(HOST_OS),Darwin)
ARM64_MACOS_SYSROOT?=$(shell xcrun --show-sdk-path)
ARM64_MACOS_RESOURCE_INC?=$(shell cc -print-resource-dir)/include
# the Linux sysroot, which tools/sysroot.sh fills from Docker; also used by the x86_64 tests
# where tools/sysroot.sh puts it: under XDG_DATA_HOME, since the compiler depends on it
X86_64_SYSROOT?=$(or $(XDG_DATA_HOME),$(HOME)/.local/share)/jcc/x86_64-linux-musl
else
ARM64_MACOS_SYSROOT?=/
ARM64_MACOS_RESOURCE_INC?=/
X86_64_SYSROOT?=/
endif
X86_64_LINUX_SYSROOT?=$(X86_64_SYSROOT)
# gcc's own directory (its headers, crt*.o and libgcc) is under a version
# directory: whichever the sysroot has
X86_64_GCC_VER?=$(notdir $(patsubst %/include,%,$(firstword $(wildcard $(X86_64_LINUX_SYSROOT)/usr/lib/gcc/x86_64-alpine-linux-musl/*/include))))
ifeq ($(X86_64_GCC_VER),)
X86_64_GCC_VER=13.2.1
endif
X86_64_LINUX_GCC_DIR?==/usr/lib/gcc/x86_64-alpine-linux-musl/$(X86_64_GCC_VER)
TARGET_DEFS=-DJCC_ARM64_MACOS_SYSROOT='"$(ARM64_MACOS_SYSROOT)"' \
    -DJCC_ARM64_MACOS_RESOURCE_INC='"$(ARM64_MACOS_RESOURCE_INC)"' \
    -DJCC_X86_64_LINUX_SYSROOT='"$(X86_64_LINUX_SYSROOT)"' \
    -DJCC_X86_64_LINUX_GCC_DIR='"$(X86_64_LINUX_GCC_DIR)"'

SRC=$(wildcard src/*.c)
ARCH_SRC=$(wildcard src/arch/arch.*.c)
AS_SRC=$(wildcard src/as/*.c)
OBJ=$(patsubst src/%.c,obj/%.o,$(SRC)) $(patsubst src/arch/%.c,obj/%.o,$(ARCH_SRC)) $(patsubst src/as/%.c,obj/as.%.o,$(AS_SRC))
INC=$(wildcard src/*.h) $(wildcard src/as/*.h)

.PHONY: release
release: OPT:=-Os -flto -fvisibility=hidden
release: jcc

.PHONY: static
static: OPT:=-Os -flto -fvisibility=hidden -static
static: CC:=musl-gcc
static: jcc

.PHONY: debug
debug: jcc
debug: OPT:=-O0 -ggdb3 -DDEBUG

.PHONY: sanitize
sanitize: jcc
sanitize: OPT:=-O0 -ggdb3 -fsanitize=address

.PHONY: warn-everything
warn-everything: jcc
warn-everything: CC=clang -Weverything -Werror -Wno-padded -Wno-switch-enum

# the library is everything but the driver; the driver is its first client
LIB_OBJ=$(filter-out obj/main.o,$(OBJ)) y.tab.o

libjcc.a: $(LIB_OBJ) Makefile
	rm -f libjcc.a
	ar rcs libjcc.a $(LIB_OBJ)

jcc: obj/main.o libjcc.a Makefile
	$(CC) -o jcc $(CFLAGS) obj/main.o libjcc.a $(LFLAGS)

y.tab.c: src/grammar.y $(INC)
	yacc -o y.tab.c src/grammar.y

y.tab.o: y.tab.c
	$(CC) -c -o y.tab.o $(CFLAGS) y.tab.c \
		-Wno-unused-macros -Wno-missing-variable-declarations

obj/arch.%.o: src/arch/arch.%.c $(INC) $(wildcard src/arch/*.h) Makefile | obj
	$(CC) -c -o $@ $(CFLAGS) $(TARGET_DEFS) -Isrc $<

obj/as.%.o: src/as/%.c $(INC) Makefile | obj
	$(CC) -c -o $@ $(CFLAGS) -Isrc $<

obj/%.o: src/%.c $(INC) Makefile | obj
	$(CC) -c -o $@ $(CFLAGS) $<

obj:
	mkdir -p obj

# the quick suite, at -O0 and then at -O1 (test-O0 and test-O1 run one level)
.PHONY: test test-O0
test: test-O0 test-O1

test-O0: jcc
	sh tests/run.sh
	sh tests/target.sh
	sh tests/abi.sh
	sh tests/embed.sh
	sh tests/limits.sh
	sh tests/c89.sh
	sh tests/as.sh
	sh tests/as-obj.sh

# the x86_64 Linux target: cross compile on this machine, assemble here, link and run in Docker
.PHONY: test-x86_64
test-x86_64: jcc
	X86_64_SYSROOT='$(X86_64_SYSROOT)' sh tests/run-x86_64.sh
	X86_64_SYSROOT='$(X86_64_SYSROOT)' sh tests/abi.sh x86_64

.PHONY: pewter-x86_64
pewter-x86_64: jcc
	X86_64_SYSROOT='$(X86_64_SYSROOT)' sh tests/run-x86_64.sh pewter

.PHONY: cpp-check
cpp-check: jcc
	sh tests/cpp-check.sh

# the c-testsuite conformance tests (tools/c-testsuite.sh fetches them), natively and for Linux
.PHONY: c-testsuite c-testsuite-x86_64
c-testsuite: jcc
	sh tests/run-c-testsuite.sh
c-testsuite-x86_64: jcc
	X86_64_SYSROOT='$(X86_64_SYSROOT)' sh tests/run-c-testsuite.sh x86_64

# the second real program: Lua 5.4.8 and its test suite (tools/lua.sh fetches them)
.PHONY: lua lua-x86_64
lua: jcc
	sh tests/run-lua.sh
lua-x86_64: jcc
	X86_64_SYSROOT='$(X86_64_SYSROOT)' sh tests/run-lua.sh x86_64

# GCC's execute torture tests (tools/gcc-torture.sh fetches them); tests/gcc-torture.xfail lists the known failures
.PHONY: gcc-torture gcc-torture-x86_64
gcc-torture: jcc
	sh tests/run-gcc-torture.sh
gcc-torture-x86_64: jcc
	X86_64_SYSROOT='$(X86_64_SYSROOT)' sh tests/run-gcc-torture.sh x86_64

# differential testing against the host compiler on N random Csmith programs (tools/csmith.sh builds it)
N?=100
.PHONY: csmith csmith-x86_64
csmith: jcc
	N='$(N)' SEED='$(SEED)' sh tests/run-csmith.sh
csmith-x86_64: jcc
	N='$(N)' SEED='$(SEED)' X86_64_SYSROOT='$(X86_64_SYSROOT)' sh tests/run-csmith.sh x86_64

# the third real program: SQLite 3.53.4's amalgamation and shell (tools/sqlite.sh fetches them)
.PHONY: sqlite sqlite-x86_64
sqlite: jcc
	sh tests/run-sqlite.sh
sqlite-x86_64: jcc
	X86_64_SYSROOT='$(X86_64_SYSROOT)' sh tests/run-sqlite.sh x86_64

# everything: the native suites, the x86_64 ones when Docker and the sysroot are there, the same again at -O1,
# then ASan (tests/verify.sh)
.PHONY: verify
verify: jcc
	X86_64_SYSROOT='$(X86_64_SYSROOT)' N='$(N)' sh tests/verify.sh

TARGET_VARS=ARM64_MACOS_SYSROOT='$(ARM64_MACOS_SYSROOT)' ARM64_MACOS_RESOURCE_INC='$(ARM64_MACOS_RESOURCE_INC)' \
    X86_64_LINUX_SYSROOT='$(X86_64_LINUX_SYSROOT)' X86_64_LINUX_GCC_DIR='$(X86_64_LINUX_GCC_DIR)'

.PHONY: selfhost
selfhost: jcc
	$(TARGET_VARS) sh tests/selfhost.sh

# The optimizer under test. jcc-O1 is jcc with -O1 added, so that the
# suites' JCC hook takes it as it is; `make X-O1` is `make X` with
# JCC=./jcc-O1 for every suite above (test, lua, csmith-x86_64, ...).
# The self-host is the exception: its stages are built with JCCFLAGS.
jcc-O1: jcc Makefile
	printf '#!/bin/sh\nexec "$$(dirname "$$0")/jcc" -O1 "$$@"\n' >jcc-O1
	chmod +x jcc-O1

.PHONY: selfhost-O1
selfhost-O1: jcc
	JCCFLAGS=-O1 $(TARGET_VARS) sh tests/selfhost.sh

%-O1: jcc-O1
	$(MAKE) --no-print-directory $* JCC=./jcc-O1

# explicit, since `make test` is both levels and the pattern rule would recurse
.PHONY: test-O1
test-O1: jcc-O1
	$(MAKE) --no-print-directory test-O0 JCC=./jcc-O1

# the library as one file (Goal 4): dist/jcc.c and dist/jcc.h, generated by tools/amalgamate.sh
.PHONY: amalgamation
amalgamation: dist/jcc.c
dist/jcc.c dist/jcc.h: $(SRC) $(ARCH_SRC) $(AS_SRC) $(INC) $(wildcard src/arch/*.h) y.tab.c tools/amalgamate.sh
	sh tools/amalgamate.sh dist

# the amalgamation built by the host compiler and by jcc, and the embed hosts against it alone (tests/amalgamation.sh)
.PHONY: test-amalgamation
test-amalgamation: jcc amalgamation
	$(TARGET_VARS) sh tests/amalgamation.sh

# no static name may be defined in two of the library's files, or they could not share a translation unit
.PHONY: statics
statics:
	sh tests/statics.sh

.PHONY: clean
clean:
	rm -f obj/* obj-*/*
	rm -f jcc jcc-* libjcc.a
	rm -rf dist
	rm -f core
	rm -f y.tab.*
