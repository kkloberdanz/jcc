/*
 * In-process execution (Step 15 of the plan): load a relocatable object
 * that jcc's assembler wrote (or the host's cc, under -fexternal-as)
 * into this process, apply its relocations, resolve what it leaves
 * undefined against the host, and hand back addresses by name. It is a
 * static linker's work done for one object at run time, for the hosts
 * jcc targets natively: Mach-O on arm64 macOS and ELF on x86_64 Linux
 * and FreeBSD. Both readers are compiled everywhere; loading an object
 * of the other format, or on any other host, is refused with a message.
 *
 * One mapping per module, in three page-aligned parts:
 *
 *   code   the executable sections, then one stub per undefined symbol
 *          that a call names: an absolute jump through an address kept
 *          in the stub, so a call to a host function farther away than
 *          a branch reaches (arm64 bl: 128 MB, x86_64 call: 2 GB) still
 *          arrives. Calls inside the module go direct.
 *   const  the read-only sections and the GOT, read-only once relocated
 *   data   the writable sections, zero-fill ones included
 *
 * Elsewhere the code part is made read and execute once written. On
 * Apple silicon it is MAP_JIT, written with the thread's write
 * protection lifted and the instruction cache invalidated afterwards,
 * and the other two parts are an ordinary mapping laid over its tail
 * (MAP_FIXED), because a MAP_JIT page is never writable and executable
 * for one thread at once and the data must stay writable while the
 * code runs. Keeping it one region keeps every part within reach of
 * adrp (4 GB) and a 32-bit displacement (2 GB). A host with the
 * hardened runtime needs the com.apple.security.cs.allow-jit
 * entitlement for MAP_JIT.
 *
 * What an object may contain is what the backends emit plus a little
 * slack for clang's objects: no thread-local storage, no common
 * symbols, and unwind sections (__eh_frame, __compact_unwind) are not
 * loaded, so a debugger cannot unwind through the module's frames.
 */
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "jit.h"
#include "diag.h"
#include "util.h"

#if defined(__APPLE__) && defined(__aarch64__)
#define JIT_HOST_MACHO 1
/* <pthread.h> and <libkern/OSCacheControl.h>, declared here so that the amalgamation's hoisted includes stay portable */
void pthread_jit_write_protect_np(int enabled);
void sys_icache_invalidate(void *start, size_t len);
#elif defined(__x86_64__) && (defined(__linux__) || defined(__FreeBSD__))
#define JIT_HOST_ELF 1
#endif

/* ------------------------------------------------------------------ */
/* the object, read into one form for both formats                    */
/* ------------------------------------------------------------------ */

typedef enum JitKind { JIT_SKIP, JIT_CODE, JIT_CONST, JIT_DATA } JitKind;

typedef struct JitSec {
    JitKind kind;
    const unsigned char *bytes;     /* NULL: zero-fill */
    unsigned long size;
    unsigned long align;
    unsigned long objaddr;          /* Mach-O: its address in the object, which symbols are relative to */
    unsigned long reloff;           /* Mach-O: where its relocations are, and how many */
    unsigned long nreloc;
    unsigned char *addr;            /* where it was loaded; NULL when skipped */
} JitSec;

enum { JIT_UNDEF = -1, JIT_ABS = -2 };

typedef struct JitSym {
    const char *name;               /* as the object spells it (Mach-O: with the leading _) */
    int sec;                        /* an index into secs, JIT_UNDEF or JIT_ABS */
    unsigned long value;            /* the offset in sec, or the absolute value */
    int global;
    int weak;                       /* undefined and weak: 0 when nothing has it */
    int used;                       /* a relocation names it */
    long got;                       /* its GOT slot's offset in the const part, or -1 */
    long stub;                      /* its stub's offset in the code part, or -1 */
    unsigned char *addr;
} JitSym;

typedef enum JitRelKind {
    JIT_R_ABS64,                    /* S + A, 64 bits */
    JIT_R_ABS32,                    /* S + A, 32 bits, zero-extended */
    JIT_R_ABS32S,                   /* S + A, 32 bits, sign-extended */
    JIT_R_PC32,                     /* S + A - P */
    JIT_R_PLT32,                    /* L + A - P, L the stub of an undefined symbol */
    JIT_R_GOTPCREL,                 /* G + A - P, G the symbol's GOT slot */
    JIT_R_BRANCH26,                 /* arm64 b/bl to L + A */
    JIT_R_PAGE21,                   /* arm64 adrp of S + A */
    JIT_R_PAGEOFF12,                /* arm64 add/ldr/str low 12 bits of S + A */
    JIT_R_GOT_PAGE21,               /* adrp of G */
    JIT_R_GOT_PAGEOFF12             /* ldr low 12 bits of G */
} JitRelKind;

typedef struct JitRel {
    int sec;                        /* the section patched */
    unsigned long off;
    JitRelKind kind;
    int sym;                        /* the symbol, or -1: relative to section `target` */
    int target;
    long addend;
} JitRel;

typedef struct JitObj {
    int macho;
    const unsigned char *p;
    size_t len;
    JitSec *secs;
    size_t nsecs;
    JitSym *syms;
    size_t nsyms;
    JitRel *rels;
    size_t nrels;
    size_t relcap;
} JitObj;

/* a defined global symbol of a loaded module, by the C name */
typedef struct JitExport {
    const char *name;
    void *addr;
} JitExport;

struct JccJit {
    unsigned char *base;
    size_t size;
    JitExport *exports;             /* sorted by name */
    size_t nexports;
    char *names;
};

static const Loc jit_noloc = { NULL, 0, 0 };

static unsigned long jit_rd16(const unsigned char *p) {
    return (unsigned long)p[0] | (unsigned long)p[1] << 8;
}

static unsigned long jit_rd32(const unsigned char *p) {
    return jit_rd16(p) | jit_rd16(p + 2) << 16;
}

static unsigned long jit_rd64(const unsigned char *p) {
    return jit_rd32(p) | jit_rd32(p + 4) << 32;
}

static void jit_wr32(unsigned char *p, unsigned long v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void jit_wr64(unsigned char *p, unsigned long v) {
    jit_wr32(p, v & 0xfffffffful);
    jit_wr32(p + 4, v >> 32);
}

/* `n` bytes at `off` lie within the object */
static int jit_within(const JitObj *o, unsigned long off, unsigned long n) {
    return off <= o->len && n <= o->len - off;
}

static int jit_bad(const char *what) {
    diag_error(jit_noloc, "malformed object: %s", what);
    return -1;
}

static void jit_add_rel(JitObj *o, int sec, unsigned long off, JitRelKind kind, int sym, int target, long addend) {
    JitRel *r;
    if (o->nrels == o->relcap) {
        o->relcap = o->relcap ? o->relcap * 2 : 64;
        o->rels = xrealloc(o->rels, o->relcap * sizeof(JitRel));
    }
    r = &o->rels[o->nrels++];
    r->sec = sec;
    r->off = off;
    r->kind = kind;
    r->sym = sym;
    r->target = target;
    r->addend = addend;
}

/* the bytes a relocation of `kind` patches */
static unsigned long jit_width(JitRelKind kind) {
    return kind == JIT_R_ABS64 ? 8 : 4;
}

/* ------------------------------------------------------------------ */
/* Mach-O, arm64                                                       */
/* ------------------------------------------------------------------ */

static int jit_name16(const unsigned char *p, const char *s) {
    return strncmp((const char *)p, s, 16) == 0;
}

static int jit_macho_relocs(JitObj *o, int sec, const int *secmap, unsigned long nfilesecs) {
    const JitSec *s = &o->secs[sec];
    unsigned long i;
    long pending = 0;
    int have_pending = 0;
    if (!jit_within(o, s->reloff, s->nreloc * 8)) {
        return jit_bad("relocations outside the file");
    }
    for (i = 0; i < s->nreloc; i++) {
        const unsigned char *e = o->p + s->reloff + 8 * i;
        unsigned long addr = jit_rd32(e);
        unsigned long info = jit_rd32(e + 4);
        unsigned long symnum = info & 0xffffff;
        unsigned long length = (info >> 25) & 3;
        int ext = (int)((info >> 27) & 1);
        unsigned long type = info >> 28;
        long addend = have_pending ? pending : 0;
        JitRelKind kind;
        int sym = -1;
        int target = -1;
        if (addr & 0x80000000ul) {
            return jit_bad("a scattered relocation");
        }
        if (type == 10) {           /* ARM64_RELOC_ADDEND: the next entry's addend, 24 bits signed */
            pending = (long)(symnum ^ 0x800000) - 0x800000;
            have_pending = 1;
            continue;
        }
        have_pending = 0;
        switch (type) {
            case 0: kind = length == 3 ? JIT_R_ABS64 : JIT_R_ABS32; break;
            case 2: kind = JIT_R_BRANCH26; break;
            case 3: kind = JIT_R_PAGE21; break;
            case 4: kind = JIT_R_PAGEOFF12; break;
            case 5: kind = JIT_R_GOT_PAGE21; break;
            case 6: kind = JIT_R_GOT_PAGEOFF12; break;
            case 8: case 9:
                diag_error(jit_noloc, "thread-local storage is not supported in a loaded module");
                return -1;
            default:
                diag_error(jit_noloc, "Mach-O relocation type %lu is not supported in a loaded module", type);
                return -1;
        }
        if (addr > s->size || jit_width(kind) > s->size - addr || !s->bytes) {
            return jit_bad("a relocation outside its section");
        }
        if (kind == JIT_R_ABS64) {
            addend = (long)jit_rd64(s->bytes + addr);      /* UNSIGNED keeps its addend in the bytes */
        } else if (kind == JIT_R_ABS32) {
            addend = (long)jit_rd32(s->bytes + addr);
        }
        if (ext) {
            if (symnum >= o->nsyms) {
                return jit_bad("a relocation names a symbol that is not there");
            }
            sym = (int)symnum;
            o->syms[sym].used = 1;
        } else {
            /* section-relative: the bytes hold an address in the object's layout */
            if (kind != JIT_R_ABS64 && kind != JIT_R_ABS32) {
                return jit_bad("a section-relative relocation that is not UNSIGNED");
            }
            if (symnum == 0 || symnum > nfilesecs || secmap[symnum - 1] < 0) {
                return jit_bad("a relocation names a section that is not there");
            }
            target = secmap[symnum - 1];
            addend -= (long)o->secs[target].objaddr;
        }
        jit_add_rel(o, sec, addr, kind, sym, target, addend);
    }
    return 0;
}

static int jit_macho(JitObj *o) {
    const unsigned char *p = o->p;
    unsigned long ncmds;
    unsigned long off;
    unsigned long i;
    unsigned long k;
    unsigned long symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    unsigned long nfilesecs = 0;
    int *secmap;                    /* the file's section ordinal - 1 -> its index in secs */
    int rc = -1;

    if (o->len < 32 || jit_rd32(p + 4) != 0x0100000cul || jit_rd32(p + 12) != 1) {
        diag_error(jit_noloc, "not an arm64 Mach-O relocatable object");
        return -1;
    }
    ncmds = jit_rd32(p + 16);

    /* the sections first: count them, then read them */
    for (k = 0; k < 2; k++) {
        off = 32;
        for (i = 0; i < ncmds; i++) {
            unsigned long cmd, size;
            if (!jit_within(o, off, 8)) {
                return jit_bad("load commands outside the file");
            }
            cmd = jit_rd32(p + off);
            size = jit_rd32(p + off + 4);
            if (size < 8 || !jit_within(o, off, size)) {
                return jit_bad("a load command outside the file");
            }
            if (cmd == 0x19 && size >= 72) {          /* LC_SEGMENT_64 */
                unsigned long n = jit_rd32(p + off + 64);
                unsigned long j;
                if (72 + 80 * n > size) {
                    return jit_bad("a segment's sections outside its command");
                }
                for (j = 0; j < n; j++) {
                    const unsigned char *h = p + off + 72 + 80 * j;
                    JitSec *s;
                    unsigned long flags = jit_rd32(h + 64);
                    unsigned long type = flags & 0xff;
                    if (k == 0) {
                        o->nsecs++;
                        continue;
                    }
                    s = &o->secs[o->nsecs++];
                    s->objaddr = jit_rd64(h + 32);
                    s->size = jit_rd64(h + 40);
                    s->align = jit_rd32(h + 52) < 16 ? 1ul << jit_rd32(h + 52) : 0;
                    s->reloff = jit_rd32(h + 56);
                    s->nreloc = jit_rd32(h + 60);
                    if (s->align == 0) {
                        return jit_bad("a section aligned past 32 KB");
                    }
                    if (!jit_name16(h + 16, "__TEXT") && !jit_name16(h + 16, "__DATA")) {
                        continue;           /* __LD,__compact_unwind, __DWARF */
                    }
                    if (jit_name16(h, "__eh_frame")) {
                        continue;
                    }
                    if (type >= 0x11 && type <= 0x15) {
                        diag_error(jit_noloc, "thread-local storage is not supported in a loaded module");
                        return -1;
                    }
                    if (flags & 0x80000400ul) {
                        s->kind = JIT_CODE;
                    } else if (jit_name16(h + 16, "__TEXT")) {
                        s->kind = JIT_CONST;
                    } else {
                        s->kind = JIT_DATA;
                    }
                    if (type == 1 || type == 0xc) {   /* S_ZEROFILL, S_GB_ZEROFILL */
                        s->bytes = NULL;
                    } else if (jit_within(o, jit_rd32(h + 48), s->size)) {
                        s->bytes = p + jit_rd32(h + 48);
                    } else {
                        return jit_bad("a section's contents outside the file");
                    }
                }
            } else if (cmd == 0x2 && size >= 24 && k == 0) {   /* LC_SYMTAB */
                symoff = jit_rd32(p + off + 8);
                nsyms = jit_rd32(p + off + 12);
                stroff = jit_rd32(p + off + 16);
                strsize = jit_rd32(p + off + 20);
            }
            off += size;
        }
        if (k == 0) {
            nfilesecs = o->nsecs;
            o->secs = xcalloc(o->nsecs + 1, sizeof(JitSec));
            o->nsecs = 0;
        }
    }
    secmap = xmalloc((nfilesecs + 1) * sizeof(int));
    for (i = 0; i < nfilesecs; i++) {
        secmap[i] = (int)i;
    }

    /* the symbols */
    if (!jit_within(o, symoff, nsyms * 16) || !jit_within(o, stroff, strsize)) {
        jit_bad("the symbol table outside the file");
        goto done;
    }
    o->syms = xcalloc(nsyms + 1, sizeof(JitSym));
    o->nsyms = nsyms;
    for (i = 0; i < nsyms; i++) {
        const unsigned char *e = p + symoff + 16 * i;
        JitSym *s = &o->syms[i];
        unsigned long strx = jit_rd32(e);
        unsigned type = e[4];
        unsigned long sect = e[5];
        unsigned long value = jit_rd64(e + 8);
        if (strx >= strsize || memchr(p + stroff + strx, 0, strsize - strx) == NULL) {
            jit_bad("a symbol name outside the string table");
            goto done;
        }
        s->name = (const char *)p + stroff + strx;
        s->got = -1;
        s->stub = -1;
        if (type & 0xe0) {          /* a debugging entry */
            s->sec = JIT_ABS;
            continue;
        }
        s->global = (type & 1) != 0;
        switch (type & 0x0e) {
            case 0x0:               /* N_UNDF */
                if (value != 0) {
                    diag_error(jit_noloc, "common symbol %s is not supported in a loaded module", s->name);
                    goto done;
                }
                s->sec = JIT_UNDEF;
                s->weak = (jit_rd16(e + 6) & 0x40) != 0;   /* N_WEAK_REF */
                break;
            case 0x2:               /* N_ABS */
                s->sec = JIT_ABS;
                s->value = value;
                break;
            case 0xe:               /* N_SECT */
                if (sect == 0 || sect > nfilesecs) {
                    jit_bad("a symbol in a section that is not there");
                    goto done;
                }
                s->sec = secmap[sect - 1];
                s->value = value - o->secs[s->sec].objaddr;
                break;
            default:
                s->sec = JIT_ABS;   /* N_INDR, N_PBUD: not in an object jcc or clang writes */
                break;
        }
    }

    /* the relocations of the sections that are loaded */
    for (i = 0; i < o->nsecs; i++) {
        if (o->secs[i].kind != JIT_SKIP && jit_macho_relocs(o, (int)i, secmap, nfilesecs) != 0) {
            goto done;
        }
    }
    rc = 0;
done:
    free(secmap);
    return rc;
}

/* ------------------------------------------------------------------ */
/* ELF64, x86_64                                                       */
/* ------------------------------------------------------------------ */

static int jit_elf(JitObj *o) {
    const unsigned char *p = o->p;
    unsigned long shoff;
    unsigned long shnum;
    unsigned long i;
    const unsigned char *symtab = NULL;
    unsigned long nsyms = 0;
    const unsigned char *strtab = NULL;
    unsigned long strsize = 0;

    if (o->len < 64 || p[4] != 2 || p[5] != 1 || jit_rd16(p + 16) != 1 || jit_rd16(p + 18) != 62) {
        diag_error(jit_noloc, "not an x86_64 ELF64 relocatable object");
        return -1;
    }
    shoff = jit_rd64(p + 40);
    shnum = jit_rd16(p + 60);
    if (jit_rd16(p + 58) != 64 || !jit_within(o, shoff, 64 * shnum)) {
        return jit_bad("section headers outside the file");
    }

    /* the sections, one for each header so that symbols' section indexes need no map */
    o->secs = xcalloc(shnum + 1, sizeof(JitSec));
    o->nsecs = shnum;
    for (i = 0; i < shnum; i++) {
        const unsigned char *h = p + shoff + 64 * i;
        JitSec *s = &o->secs[i];
        unsigned long type = jit_rd32(h + 4);
        unsigned long flags = jit_rd64(h + 8);
        unsigned long off = jit_rd64(h + 24);
        s->size = jit_rd64(h + 32);
        s->align = jit_rd64(h + 48);
        if (s->align == 0) {
            s->align = 1;
        }
        if (s->align & (s->align - 1) || s->align > 32768) {
            return jit_bad("a section alignment that is not a power of two up to 32 KB");
        }
        if (type == 2) {            /* SHT_SYMTAB */
            unsigned long link = jit_rd32(h + 40);
            const unsigned char *lh;
            if (!jit_within(o, off, s->size) || link >= shnum) {
                return jit_bad("the symbol table outside the file");
            }
            symtab = p + off;
            nsyms = s->size / 24;
            lh = p + shoff + 64 * link;
            strsize = jit_rd64(lh + 32);
            if (!jit_within(o, jit_rd64(lh + 24), strsize)) {
                return jit_bad("the string table outside the file");
            }
            strtab = p + jit_rd64(lh + 24);
        }
        if (!(flags & 2)) {         /* SHF_ALLOC */
            continue;
        }
        if (flags & 0x400) {        /* SHF_TLS */
            diag_error(jit_noloc, "thread-local storage is not supported in a loaded module");
            return -1;
        }
        s->kind = flags & 4 ? JIT_CODE : flags & 1 ? JIT_DATA : JIT_CONST;
        if (type == 8) {            /* SHT_NOBITS */
            s->bytes = NULL;
        } else if (jit_within(o, off, s->size)) {
            s->bytes = p + off;
        } else {
            return jit_bad("a section's contents outside the file");
        }
    }

    /* the symbols */
    o->syms = xcalloc(nsyms + 1, sizeof(JitSym));
    o->nsyms = nsyms;
    for (i = 0; i < nsyms; i++) {
        const unsigned char *e = symtab + 24 * i;
        JitSym *s = &o->syms[i];
        unsigned long name = jit_rd32(e);
        unsigned bind = e[4] >> 4;
        unsigned long shndx = jit_rd16(e + 6);
        if (name >= strsize || memchr(strtab + name, 0, strsize - name) == NULL) {
            return jit_bad("a symbol name outside the string table");
        }
        s->name = (const char *)strtab + name;
        s->value = jit_rd64(e + 8);
        s->global = bind == 1 || bind == 2;
        s->got = -1;
        s->stub = -1;
        if (shndx == 0) {
            s->sec = JIT_UNDEF;
            s->weak = bind == 2;
        } else if (shndx == 0xfff1) {
            s->sec = JIT_ABS;
        } else if (shndx == 0xfff2) {
            diag_error(jit_noloc, "common symbol %s is not supported in a loaded module", s->name);
            return -1;
        } else if (shndx < shnum) {
            s->sec = (int)shndx;
        } else {
            return jit_bad("a symbol in a section that is not there");
        }
    }

    /* the relocations of the sections that are loaded */
    for (i = 0; i < shnum; i++) {
        const unsigned char *h = p + shoff + 64 * i;
        unsigned long type = jit_rd32(h + 4);
        unsigned long off = jit_rd64(h + 24);
        unsigned long size = jit_rd64(h + 32);
        unsigned long info = jit_rd32(h + 44);
        unsigned long j;
        if (type == 9) {
            return jit_bad("REL relocations in an x86_64 object");
        }
        if (type != 4) {            /* SHT_RELA */
            continue;
        }
        if (info >= shnum || !jit_within(o, off, size)) {
            return jit_bad("relocations outside the file");
        }
        if (o->secs[info].kind == JIT_SKIP) {
            continue;
        }
        for (j = 0; j + 24 <= size; j += 24) {
            const unsigned char *e = p + off + j;
            unsigned long where = jit_rd64(e);
            unsigned long rtype = jit_rd32(e + 8);
            unsigned long sym = jit_rd32(e + 12);
            long addend = (long)jit_rd64(e + 16);
            const JitSec *s = &o->secs[info];
            JitRelKind kind;
            switch (rtype) {
                case 1: kind = JIT_R_ABS64; break;
                case 2: kind = JIT_R_PC32; break;
                case 4: kind = JIT_R_PLT32; break;
                case 9: case 41: case 42: kind = JIT_R_GOTPCREL; break;   /* GOTPCREL, GOTPCRELX, REX_GOTPCRELX */
                case 10: kind = JIT_R_ABS32; break;
                case 11: kind = JIT_R_ABS32S; break;
                case 19: case 20: case 21: case 22: case 23:
                    diag_error(jit_noloc, "thread-local storage is not supported in a loaded module");
                    return -1;
                default:
                    diag_error(jit_noloc, "ELF relocation type %lu is not supported in a loaded module", rtype);
                    return -1;
            }
            if (sym >= nsyms || sym == 0) {
                return jit_bad("a relocation names a symbol that is not there");
            }
            if (where > s->size || jit_width(kind) > s->size - where || !s->bytes) {
                return jit_bad("a relocation outside its section");
            }
            o->syms[sym].used = 1;
            jit_add_rel(o, (int)info, where, kind, (int)sym, -1, addend);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* loading                                                             */
/* ------------------------------------------------------------------ */

static unsigned long jit_round(unsigned long n, unsigned long align) {
    return (n + align - 1) & ~(align - 1);
}

/* the C name of a symbol: Mach-O prefixes an underscore */
static const char *jit_cname(const JitObj *o, const char *name) {
    return o->macho && name[0] == '_' ? name + 1 : name;
}

static void *jit_host_symbol(const char *name) {
#ifdef RTLD_DEFAULT
    return dlsym(RTLD_DEFAULT, name);
#else
    /* glibc and musl define RTLD_DEFAULT only for _GNU_SOURCE; the main program's handle searches the same scope */
    void *self = dlopen(NULL, RTLD_LAZY);
    void *addr = self ? dlsym(self, name) : NULL;
    if (self) {
        dlclose(self);
    }
    return addr;
#endif
}

static int jit_by_name(const void *x, const void *y) {
    return strcmp(((const JitExport *)x)->name, ((const JitExport *)y)->name);
}

/* patch one relocation; P, S, G and L as in the ELF psABI */
static int jit_apply(const JitObj *o, const JitRel *r, unsigned char *code, unsigned char *cnst) {
    unsigned char *at = o->secs[r->sec].addr + r->off;
    unsigned long P = (unsigned long)at;
    unsigned long S;
    unsigned long G = 0;
    unsigned long L;
    unsigned long insn = jit_rd32(at);
    const char *name = "a section";
    long v;
    if (r->sym >= 0) {
        const JitSym *s = &o->syms[r->sym];
        name = jit_cname(o, s->name);
        S = (unsigned long)s->addr;
        G = s->got >= 0 ? (unsigned long)(cnst + s->got) : 0;
        L = s->stub >= 0 ? (unsigned long)(code + s->stub) : S;
    } else {
        S = (unsigned long)o->secs[r->target].addr;
        L = S;
    }
    switch (r->kind) {
        case JIT_R_ABS64:
            jit_wr64(at, S + (unsigned long)r->addend);
            return 0;
        case JIT_R_ABS32:
            v = (long)(S + (unsigned long)r->addend);
            if (v < 0 || v > 0xffffffffl) {
                break;
            }
            jit_wr32(at, (unsigned long)v);
            return 0;
        case JIT_R_ABS32S:
            v = (long)(S + (unsigned long)r->addend);
            if (v < -0x80000000l || v > 0x7fffffffl) {
                break;
            }
            jit_wr32(at, (unsigned long)v);
            return 0;
        case JIT_R_PC32:
        case JIT_R_PLT32:
        case JIT_R_GOTPCREL:
            v = (long)((r->kind == JIT_R_PC32 ? S : r->kind == JIT_R_PLT32 ? L : G) + (unsigned long)r->addend - P);
            if (v < -0x80000000l || v > 0x7fffffffl) {
                break;
            }
            jit_wr32(at, (unsigned long)v);
            return 0;
        case JIT_R_BRANCH26:
            v = (long)(L + (unsigned long)r->addend - P);
            if (v & 3 || v < -(1l << 27) || v >= (1l << 27)) {
                break;
            }
            jit_wr32(at, (insn & 0xfc000000ul) | (((unsigned long)v >> 2) & 0x3fffffful));
            return 0;
        case JIT_R_PAGE21:
        case JIT_R_GOT_PAGE21:
            v = (long)(((r->kind == JIT_R_PAGE21 ? S + (unsigned long)r->addend : G) & ~0xffful) - (P & ~0xffful)) >> 12;
            if (v < -(1l << 20) || v >= (1l << 20)) {
                break;
            }
            insn &= ~((3ul << 29) | (0x7fffful << 5));
            insn |= ((unsigned long)v & 3) << 29 | (((unsigned long)v >> 2) & 0x7fffful) << 5;
            jit_wr32(at, insn);
            return 0;
        case JIT_R_PAGEOFF12:
        case JIT_R_GOT_PAGEOFF12: {
            unsigned long lo = (r->kind == JIT_R_PAGEOFF12 ? S + (unsigned long)r->addend : G) & 0xfff;
            unsigned shift = 0;
            if ((insn & 0x3b000000ul) == 0x39000000ul) {
                /* a load or store with an unsigned offset, scaled by the access size; a 128-bit SIMD one by 16 */
                shift = (unsigned)(insn >> 30);
                if ((insn & 0x04800000ul) == 0x04800000ul && shift == 0) {
                    shift = 4;
                }
            }
            if (lo & ((1ul << shift) - 1)) {
                diag_error(jit_noloc, "relocation against %s: offset %lu is not aligned for the access", name, lo);
                return -1;
            }
            jit_wr32(at, (insn & ~(0xffful << 10)) | (lo >> shift) << 10);
            return 0;
        }
    }
    diag_error(jit_noloc, "relocation against %s is out of range", name);
    return -1;
}

static void jit_free_obj(JitObj *o) {
    free(o->secs);
    free(o->syms);
    free(o->rels);
}

JccJit *jit_load(const char *obj, size_t len, JccResolveFn resolve, void *user) {
    JitObj o;
    JccJit *j = NULL;
    unsigned long pagesize = (unsigned long)sysconf(_SC_PAGESIZE);
    unsigned long code_size = 0, const_size = 0, data_size = 0;
    unsigned long got_off, stub_off;
    unsigned long nstubs = 0, ngot = 0, nexports = 0, names_size = 0;
    unsigned long pass;
    unsigned char *base;
    unsigned char *code, *cnst, *data;
    size_t i;
    int errors = 0;

    memset(&o, 0, sizeof(o));
    o.p = (const unsigned char *)obj;
    o.len = len;
    if (len >= 4 && jit_rd32(o.p) == 0xfeedfacful) {
        o.macho = 1;
    } else if (len < 4 || memcmp(obj, "\177ELF", 4) != 0) {
        diag_error(jit_noloc, "not a relocatable object (neither Mach-O nor ELF)");
        return NULL;
    }
#if defined(JIT_HOST_MACHO)
    if (!o.macho) {
        diag_error(jit_noloc, "an ELF object cannot be loaded on this host, which runs Mach-O (arm64 macOS)");
        return NULL;
    }
#elif defined(JIT_HOST_ELF)
    if (o.macho) {
        diag_error(jit_noloc, "a Mach-O object cannot be loaded on this host, which runs ELF (x86_64)");
        return NULL;
    }
#else
    diag_error(jit_noloc, "in-process execution needs arm64 macOS or x86_64 Linux or FreeBSD as the host");
    return NULL;
#endif
    if ((o.macho ? jit_macho(&o) : jit_elf(&o)) != 0) {
        jit_free_obj(&o);
        return NULL;
    }

    /* a GOT slot for every symbol a GOT relocation names, a stub for every undefined one a call names */
    for (i = 0; i < o.nrels; i++) {
        const JitRel *r = &o.rels[i];
        JitSym *s = r->sym >= 0 ? &o.syms[r->sym] : NULL;
        if (!s) {
            continue;
        }
        if ((r->kind == JIT_R_GOTPCREL || r->kind == JIT_R_GOT_PAGE21 || r->kind == JIT_R_GOT_PAGEOFF12) && s->got < 0) {
            s->got = (long)(8 * ngot++);
        }
        if ((r->kind == JIT_R_PLT32 || r->kind == JIT_R_BRANCH26) && s->sec == JIT_UNDEF && s->stub < 0) {
            s->stub = (long)(16 * nstubs++);
        }
    }

    /*
     * The layout: each part's sections at offsets within it (kept in
     * `addr` until the mapping exists), the stubs after the code and the
     * GOT after the read-only sections.
     */
    for (pass = JIT_CODE; pass <= JIT_DATA; pass++) {
        unsigned long *size = pass == JIT_CODE ? &code_size : pass == JIT_CONST ? &const_size : &data_size;
        for (i = 0; i < o.nsecs; i++) {
            JitSec *s = &o.secs[i];
            if (s->kind != (JitKind)pass) {
                continue;
            }
            *size = jit_round(*size, s->align);
            s->addr = (unsigned char *)*size;
            *size += s->size;
        }
    }
    stub_off = code_size = jit_round(code_size, 16);
    code_size = jit_round(code_size + 16 * nstubs, pagesize);
    got_off = const_size = jit_round(const_size, 8);
    const_size = jit_round(const_size + 8 * ngot, pagesize);
    data_size = jit_round(data_size, pagesize);
    if (code_size == 0) {
        code_size = pagesize;       /* a module with no code still gets a mapping to own */
    }

    /* the exports, allocated before the mapping so that running out of memory cannot leak it */
    for (i = 0; i < o.nsyms; i++) {
        const JitSym *s = &o.syms[i];
        if (s->global && s->sec >= 0) {
            nexports++;
            names_size += strlen(jit_cname(&o, s->name)) + 1;
        }
    }
    j = xcalloc(1, sizeof(*j));
    j->exports = xmalloc((nexports + 1) * sizeof(JitExport));
    j->names = xmalloc(names_size + 1);

    /* the mapping */
    j->size = code_size + const_size + data_size;
#if defined(JIT_HOST_MACHO)
    base = mmap(NULL, j->size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (base != MAP_FAILED && const_size + data_size
        && mmap(base + code_size, const_size + data_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED) {
        munmap(base, j->size);
        base = MAP_FAILED;
    }
#else
    base = mmap(NULL, j->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
    if (base == MAP_FAILED) {
        diag_error(jit_noloc, "cannot map %lu bytes for the module", (unsigned long)j->size);
        free(j->exports);
        free(j->names);
        free(j);
        jit_free_obj(&o);
        return NULL;
    }
    j->base = base;
    code = base;
    cnst = base + code_size;
    data = cnst + const_size;
#if defined(JIT_HOST_MACHO)
    pthread_jit_write_protect_np(0);
#endif

    /* the sections' contents in place (the mapping starts zeroed, which is the zero-fill) */
    for (i = 0; i < o.nsecs; i++) {
        JitSec *s = &o.secs[i];
        unsigned char *part = s->kind == JIT_CODE ? code : s->kind == JIT_CONST ? cnst : data;
        if (s->kind == JIT_SKIP) {
            s->addr = NULL;
            continue;
        }
        s->addr = part + (unsigned long)s->addr;
        if (s->bytes && s->size) {
            memcpy(s->addr, s->bytes, s->size);
        }
    }

    /* the symbols' addresses: the module's own, then the host's for the undefined ones */
    for (i = 0; i < o.nsyms; i++) {
        JitSym *s = &o.syms[i];
        if (s->sec >= 0) {
            s->addr = o.secs[s->sec].addr ? o.secs[s->sec].addr + s->value : NULL;
        } else if (s->sec == JIT_ABS) {
            s->addr = (unsigned char *)s->value;
        } else if (s->used) {
            const char *name = jit_cname(&o, s->name);
            s->addr = resolve ? resolve(user, name) : NULL;
            if (!s->addr) {
                s->addr = jit_host_symbol(name);
            }
            if (!s->addr && !s->weak) {
                diag_error(jit_noloc, "undefined symbol %s: neither the resolver nor the process has it", name);
                errors++;
            }
        }
    }

    /* the GOT, the stubs, the relocations */
    if (!errors) {
        for (i = 0; i < o.nsyms; i++) {
            const JitSym *s = &o.syms[i];
            unsigned char *stub;
            if (s->got >= 0) {
                jit_wr64(cnst + got_off + (unsigned long)s->got, (unsigned long)s->addr);
            }
            if (s->stub < 0) {
                continue;
            }
            stub = code + stub_off + (unsigned long)s->stub;
            if (o.macho) {
                jit_wr32(stub, 0x58000050ul);              /* ldr x16, #8 */
                jit_wr32(stub + 4, 0xd61f0200ul);          /* br x16 */
                jit_wr64(stub + 8, (unsigned long)s->addr);
            } else {
                static const unsigned char jmp[8] = { 0xff, 0x25, 0, 0, 0, 0, 0xcc, 0xcc };   /* jmp *0(%rip) */
                memcpy(stub, jmp, 6);
                jit_wr64(stub + 6, (unsigned long)s->addr);
                memcpy(stub + 14, jmp + 6, 2);
            }
        }
        for (i = 0; i < o.nrels; i++) {
            if (jit_apply(&o, &o.rels[i], code + stub_off, cnst + got_off) != 0) {
                errors++;
            }
        }
    }

    /* the protections the parts keep */
#if defined(JIT_HOST_MACHO)
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(code, code_size);
#else
    if (!errors && mprotect(code, code_size, PROT_READ | PROT_EXEC) != 0) {
        diag_error(jit_noloc, "cannot make the module's code executable");
        errors++;
    }
#endif
    if (!errors && const_size && mprotect(cnst, const_size, PROT_READ) != 0) {
        diag_error(jit_noloc, "cannot make the module's constants read-only");
        errors++;
    }

    if (errors) {
        jit_free_obj(&o);
        jcc_jit_unload(j);
        return NULL;
    }

    /* the defined globals by name */
    {
        char *n = j->names;
        for (i = 0; i < o.nsyms; i++) {
            const JitSym *s = &o.syms[i];
            const char *name;
            if (!s->global || s->sec < 0) {
                continue;
            }
            name = jit_cname(&o, s->name);
            strcpy(n, name);
            j->exports[j->nexports].name = n;
            j->exports[j->nexports].addr = s->addr;
            j->nexports++;
            n += strlen(name) + 1;
        }
        qsort(j->exports, j->nexports, sizeof(JitExport), jit_by_name);
    }
    jit_free_obj(&o);
    return j;
}

void *jcc_jit_symbol(const JccJit *j, const char *name) {
    JitExport key;
    const JitExport *e;
    if (!j || !name) {
        return NULL;
    }
    key.name = name;
    key.addr = NULL;
    e = bsearch(&key, j->exports, j->nexports, sizeof(JitExport), jit_by_name);
    return e ? e->addr : NULL;
}

void jcc_jit_unload(JccJit *j) {
    if (!j) {
        return;
    }
    if (j->base) {
        munmap(j->base, j->size);
    }
    free(j->exports);
    free(j->names);
    free(j);
}

#undef JIT_HOST_MACHO
#undef JIT_HOST_ELF
