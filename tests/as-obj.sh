#!/bin/sh
# The assembler's object writers (Step 14, pieces 2 and 3): every .s kcc's
# backends produce is assembled by both `cc -c` and `kcc -c`, and the
# objects must agree on every section's bytes, the symbol table and the
# relocations. The reference is clang's output for the same text minus
# the .cfi_startproc/.cfi_endproc lines, since kcc writes no unwind
# tables (clang's would describe every function as frameless anyway).
# The arm64 Mach-O comparison runs on an arm64 host; the x86_64 ELF one
# runs anywhere clang can target x86_64-linux-musl (the sysroot is only
# needed to compile the C tests to assembly). Each compares the assembly
# of tests/*.c and tests/abi/*.c, then the corpus tools/as-corpus.sh
# collects when it is present. From the repository root (make test does).
cd "$(dirname "$0")/.." || exit 1
KCC=${KCC:-./kcc}
DATA=${KCC_DATA:-${XDG_DATA_HOME:-$HOME/.local/share}/kcc}
SYSROOT=${X86_64_SYSROOT:-$DATA/x86_64-linux-musl}
JOBS=${JOBS:-8}
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
export KCC tmp

# dump-macho OBJ: the parts compared (sections, their contents, symbols, relocations)
cat >"$tmp/dump-macho.sh" <<'DUMP'
#!/bin/sh
o=$1
otool -l "$o" | awk '/^Section/{s=1} s&&/sectname|segname|  size|  align|   type|attributes|   addr/{printf "%s ", $0} /reserved2/{print ""; s=0}'
for s in "__TEXT __text" "__DATA __data" "__TEXT __const"; do otool -X -s $s "$o"; done
objdump --macho -t "$o" | sed '1,/SYMBOL TABLE/d'
otool -rv "$o" | sed 1d
DUMP

# dump-elf OBJ: the section headers (type, flags, size, link, info, alignment, entry size; not the string
# table's size nor any offset, since kcc's string table is laid out differently), contents, symbols, relocations
cat >"$tmp/dump-elf.sh" <<'DUMP'
#!/bin/sh
o=$1
shoff=$(od -A n -t u8 -j 40 -N 8 "$o" | tr -d ' ')
shnum=$(od -A n -t u2 -j 60 -N 2 "$o" | tr -d ' ')
od -A n -t u4 -v -j "$shoff" -N $((shnum * 64)) "$o" | tr '\n' ' ' | awk '{
    for (i = 1; i <= NF; i += 16) {
        type = $(i + 1); size = $(i + 8) + $(i + 9) * 4294967296
        if (type == 3) size = "-"
        print "section", type, $(i + 2), size, $(i + 10), $(i + 11), $(i + 12), $(i + 14)
    }
}'
objdump -h -t -r "$o" | sed "/file format/d; s/^\( *[0-9]* \.strtab *\)[0-9a-f]*/\1-/"
objdump -s -j .text -j .data -j .rodata "$o" 2>/dev/null | sed "/file format/d"
DUMP

# one ARCH NAME SRC.s: 0 when the two objects agree
cat >"$tmp/one.sh" <<'ONE'
#!/bin/sh
arch=$1; name=$2; src=$3
d=$tmp/$arch-$name; mkdir -p "$d"
grep -v '^\s*\.cfi_' "$src" >"$d/ref.s"
if [ "$arch" = arm64 ]; then ref="cc"; x="$KCC"; dump=dump-macho; else ref="cc --target=x86_64-linux-musl"; x="$KCC --target=x86_64-linux-musl"; dump=dump-elf; fi
$ref -c -o "$d/ref.o" "$d/ref.s" 2>"$d/cc.err" || { echo "FAIL $arch $name: cc rejects it"; head -3 "$d/cc.err"; exit 1; }
$x -c -o "$d/kcc.o" "$src" 2>"$d/kcc.err" || { echo "FAIL $arch $name: kcc rejects it"; head -3 "$d/kcc.err"; exit 1; }
sh "$tmp/$dump.sh" "$d/ref.o" >"$d/ref.dump"
sh "$tmp/$dump.sh" "$d/kcc.o" >"$d/kcc.dump"
if ! cmp -s "$d/ref.dump" "$d/kcc.dump"; then
    echo "FAIL $arch $name: the objects differ"; diff "$d/ref.dump" "$d/kcc.dump" | head -8 | sed 's/^/    /'; exit 1
fi
rm -rf "$d"
ONE

fail=0
for arch in arm64 x86_64; do
    if [ "$arch" = arm64 ]; then
        case "$(uname -m)" in arm64) ;; *) echo "arm64: skipped (not an arm64 host)"; continue ;; esac
        X="$KCC"
    else
        X="$KCC --target=x86_64-linux-musl -isysroot $SYSROOT"
        [ -d "$SYSROOT/usr/include" ] || X=""
    fi
    n=0
    if [ -n "$X" ]; then
        for src in tests/*.c tests/abi/*.c; do
            name=$(basename "$src" .c)
            $X -S -o "$tmp/$name.$arch.s" "$src" 2>/dev/null || continue
            n=$((n + 1))
            sh "$tmp/one.sh" "$arch" "$name" "$tmp/$name.$arch.s" || fail=$((fail + 1))
        done
    fi
    echo "$arch: compared $n test programs"
    if [ -d "$DATA/as-corpus/$arch" ]; then
        ls "$DATA/as-corpus/$arch"/*.s | xargs -P "$JOBS" -n 1 sh -c 'sh "$tmp/one.sh" "'"$arch"'" "corpus-$(basename "$0" .s)" "$0"' >"$tmp/corpus.out" 2>&1
        cat "$tmp/corpus.out"
        c=$(grep -c '^FAIL' "$tmp/corpus.out")
        fail=$((fail + c))
        echo "$arch: corpus of $(ls "$DATA/as-corpus/$arch"/*.s | wc -l | tr -d ' ') files, $c failed"
    fi
done

if [ "$fail" -eq 0 ]; then
    echo "AS-OBJ OK"
else
    echo "AS-OBJ: $fail failed"
    exit 1
fi
