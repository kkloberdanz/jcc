#!/bin/sh
# Everything, in one run: the native suites, then the x86_64 Linux ones
# (skipped with a note when Docker, the sysroot or a program is missing),
# then both again at -O1 (the make X-O1 targets), then an AddressSanitizer
# build of the compiler in a scratch copy running the native suites again. Each stage is a make target or a script that
# exits 0 to pass and 2 when it cannot run (a suite that was not fetched,
# Docker not running); anything else fails. One table at the end, one log
# per stage under the directory VERIFY_LOGS (default: a temporary one,
# kept when a stage fails). The runners compile and run tests JOBS at a
# time (default: the number of processors), so the native stages take
# about two minutes and the whole set, with the -O1 repeat, about nine.
# Run from the repository root (make verify does).
cd "$(dirname "$0")/.." || exit 1
LOGS=${VERIFY_LOGS:-$(mktemp -d)}
mkdir -p "$LOGS"
DATA=${XDG_DATA_HOME:-$HOME/.local/share}/kcc
SYSROOT=${X86_64_SYSROOT:-$DATA/x86_64-linux-musl}
N=${N:-100}
failed=0
table=""

stage() {    # stage NAME COMMAND...: run it, record PASS, FAIL or SKIP with its last line
    name=$1; shift
    start=$(date +%s)
    printf '%-22s ' "$name"
    "$@" >"$LOGS/$name.log" 2>&1
    rc=$?
    secs=$(( $(date +%s) - start ))
    last=$(grep -v '^$' "$LOGS/$name.log" | grep -v '^make:' | tail -1 | cut -c1-70)
    case $rc in
        0) verdict=PASS ;;
        2) verdict=SKIP ;;
        *) verdict=FAIL; failed=$((failed + 1)) ;;
    esac
    printf '%s %4ds  %s\n' "$verdict" "$secs" "$last"
    table="$table$(printf '%-22s %s %4ds  %s' "$name" "$verdict" "$secs" "$last")
"
}

skip() {    # skip NAME REASON
    printf '%-22s SKIP    0s  %s\n' "$1" "$2"
    table="$table$(printf '%-22s SKIP    0s  %s' "$1" "$2")
"
}

echo "kcc verify: logs in $LOGS"
make -s kcc >"$LOGS/build.log" 2>&1 || { echo "the build failed:"; tail -20 "$LOGS/build.log"; exit 1; }

# native
stage test          make test-O0
stage selfhost      make selfhost
stage statics       sh tests/statics.sh
stage amalgamation  make -s test-amalgamation
stage cpp-check     make cpp-check
stage c-testsuite   sh tests/run-c-testsuite.sh
stage gcc-torture   sh tests/run-gcc-torture.sh
stage csmith        env N="$N" sh tests/run-csmith.sh
stage lua           sh tests/run-lua.sh
stage sqlite        sh tests/run-sqlite.sh

# x86_64 Linux: cross compiled here, run in Docker
if ! [ -d "$SYSROOT/usr/include" ]; then
    skip x86_64 "no sysroot at $SYSROOT: run tools/sysroot.sh"
elif ! docker info >/dev/null 2>&1; then
    skip x86_64 "docker is not running"
else
    export X86_64_SYSROOT="$SYSROOT"
    stage test-x86_64        sh tests/run-x86_64.sh
    stage abi-x86_64         sh tests/abi.sh x86_64
    if [ -f "${PEWTER:-$HOME/src/pewter}/pewter.c" ]; then
        stage pewter-x86_64  sh tests/run-x86_64.sh pewter
    else
        skip pewter-x86_64 "no pewter.c at ${PEWTER:-$HOME/src/pewter}"
    fi
    stage c-testsuite-x86_64 sh tests/run-c-testsuite.sh x86_64
    stage gcc-torture-x86_64 sh tests/run-gcc-torture.sh x86_64
    stage csmith-x86_64      env N="$N" sh tests/run-csmith.sh x86_64
    stage lua-x86_64         sh tests/run-lua.sh x86_64
    stage sqlite-x86_64      sh tests/run-sqlite.sh x86_64
fi

# the same suites with the optimizer: `make X-O1` is `make X` with KCC=./kcc-O1
stage test-O1        make -s test-O1
stage selfhost-O1    make -s selfhost-O1
stage c-testsuite-O1 make -s c-testsuite-O1
stage gcc-torture-O1 make -s gcc-torture-O1
stage csmith-O1      env N="$N" make -s csmith-O1
stage lua-O1         make -s lua-O1
stage sqlite-O1      make -s sqlite-O1
if [ -d "$SYSROOT/usr/include" ] && docker info >/dev/null 2>&1; then
    stage test-x86_64-O1        make -s test-x86_64-O1
    stage c-testsuite-x86_64-O1 make -s c-testsuite-x86_64-O1
    stage gcc-torture-x86_64-O1 make -s gcc-torture-x86_64-O1
    stage csmith-x86_64-O1      env N="$N" make -s csmith-x86_64-O1
    stage lua-x86_64-O1         make -s lua-x86_64-O1
    stage sqlite-x86_64-O1      make -s sqlite-x86_64-O1
else
    skip x86_64-O1 "no sysroot or docker"
fi

# AddressSanitizer: the compiler itself, built in a scratch copy, over the native suites
asan_suites() {
    cp -R Makefile src tests tools "$1" || return 1
    make -C "$1" -s sanitize >/dev/null 2>&1 || { echo "the sanitizer build failed"; return 1; }
    (cd "$1" && sh tests/run.sh && sh tests/target.sh && sh tests/abi.sh && sh tests/embed.sh \
        && sh tests/limits.sh && sh tests/c89.sh) || return 1
    if [ -f "$DATA/sqlite-amalgamation-3530400/sqlite3.c" ]; then
        "$1/kcc" -std=gnu89 -S -o /dev/null "$DATA/sqlite-amalgamation-3530400/sqlite3.c" || return 1
        echo "sqlite3.c compiled under ASan"
    fi
    grep -l "AddressSanitizer\|LeakSanitizer" "$LOGS/asan.log" >/dev/null 2>&1 && return 1
    echo "ASan: no reports"
}
asan_dir=$(mktemp -d)
stage asan asan_suites "$asan_dir"
rm -rf "$asan_dir"

echo
echo "=== kcc verify: $failed failed"
printf '%s' "$table"
if [ "$failed" -eq 0 ]; then
    [ -n "${VERIFY_LOGS:-}" ] || rm -rf "$LOGS"
    exit 0
fi
echo "logs: $LOGS"
exit 1
