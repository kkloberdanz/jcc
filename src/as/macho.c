/*
 * The Mach-O relocatable object writer (arm64). The layout follows what
 * clang's integrated assembler writes for the same input so that the two
 * objects can be compared: one unnamed segment with the sections in the
 * order the unit first entered them (__text first), zero-fill sections
 * placed after the others; the symbol table with the local symbols in
 * order of first mention, then the defined external and the undefined
 * ones, each group sorted by name; relocations in descending address
 * order, each local label resolved to the nearest named symbol before it
 * (every section starts with an `ltmpN` for that) plus an ADDEND entry.
 * Differences from clang, on purpose: no __compact_unwind section (kcc
 * emits no CFI beyond .cfi_startproc/.cfi_endproc, so clang's entries
 * would say "frameless" anyway) and LC_BUILD_VERSION says macOS 11.0,
 * the first arm64 release, rather than the host's version.
 */
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "diag.h"

#define MH_MAGIC_64 0xfeedfacful
#define CPU_TYPE_ARM64 0x0100000cul
#define MH_OBJECT 1
#define LC_SEGMENT_64 0x19
#define LC_SYMTAB 0x2
#define LC_DYSYMTAB 0xb
#define LC_BUILD_VERSION 0x32
#define PLATFORM_MACOS 1
#define S_ZEROFILL 1
#define S_ATTR_PURE_INSTRUCTIONS 0x80000000ul
#define S_ATTR_SOME_INSTRUCTIONS 0x00000400ul
#define N_EXT 0x01
#define N_SECT 0x0e

enum {
    ARM64_RELOC_UNSIGNED = 0,
    ARM64_RELOC_BRANCH26 = 2,
    ARM64_RELOC_PAGE21 = 3,
    ARM64_RELOC_PAGEOFF12 = 4,
    ARM64_RELOC_GOT_LOAD_PAGE21 = 5,
    ARM64_RELOC_GOT_LOAD_PAGEOFF12 = 6,
    ARM64_RELOC_ADDEND = 10
};

typedef struct MachoBuf {
    unsigned char *p;
    size_t len;
    size_t cap;
} MachoBuf;

static void macho_grow(MachoBuf *b, size_t n) {
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2;
        b->p = realloc(b->p, b->cap);
        if (!b->p) {
            fatal("out of memory");
        }
    }
}

static void macho_put(MachoBuf *b, const void *p, size_t n) {
    macho_grow(b, n);
    memcpy(b->p + b->len, p, n);
    b->len += n;
}

static void macho_put_zero(MachoBuf *b, size_t n) {
    macho_grow(b, n);
    memset(b->p + b->len, 0, n);
    b->len += n;
}

static void put32(MachoBuf *b, unsigned long v) {
    unsigned char x[4];
    x[0] = (unsigned char)v;
    x[1] = (unsigned char)(v >> 8);
    x[2] = (unsigned char)(v >> 16);
    x[3] = (unsigned char)(v >> 24);
    macho_put(b, x, 4);
}

static void put64(MachoBuf *b, unsigned long v) {
    put32(b, v & 0xfffffffful);
    put32(b, v >> 32);
}

static void put_name(MachoBuf *b, const char *s) {
    char x[16];
    memset(x, 0, sizeof(x));
    strncpy(x, s, sizeof(x));
    macho_put(b, x, 16);
}

/* a relocation entry as written */
typedef struct MachoRel {
    unsigned long addr;
    unsigned long info;
} MachoRel;

typedef struct MSec {
    int kind;               /* SecKind */
    const char *sectname;
    const char *segname;
    unsigned long flags;
    unsigned long addr;
    unsigned long fileoff;
    unsigned long reloff;
    MachoRel *rels;
    size_t nrels;
} MSec;

static int macho_by_order(const void *x, const void *y) {
    const MSec *a = x;
    const MSec *b = y;
    return a->kind == b->kind ? 0 : (a->addr < b->addr ? -1 : 1);
}

static int by_name(const void *x, const void *y) {
    return strcmp((*(const AsmSym *const *)x)->name, (*(const AsmSym *const *)y)->name);
}

/* the symbol table: locals in order of mention, externals and undefined sorted; index[i] is sym i's entry */
static AsmSym **macho_symtab(As *a, int *index, size_t *nlocal, size_t *next, size_t *nundef) {
    AsmSym **tab = malloc((a->nsyms + 1) * sizeof(AsmSym *));
    size_t n = 0;
    size_t i;
    if (!tab) {
        fatal("out of memory");
    }
    for (i = 0; i < a->nsyms; i++) {
        AsmSym *s = &a->syms[i];
        if (s->section >= 0 && !s->global && !s->local) {
            tab[n++] = s;
        }
    }
    *nlocal = n;
    for (i = 0; i < a->nsyms; i++) {
        AsmSym *s = &a->syms[i];
        if (s->section >= 0 && s->global) {
            tab[n++] = s;
        }
    }
    *next = n - *nlocal;
    qsort(tab + *nlocal, *next, sizeof(AsmSym *), by_name);
    for (i = 0; i < a->nsyms; i++) {
        AsmSym *s = &a->syms[i];
        if (s->section < 0 && !s->local) {
            tab[n++] = s;
        }
    }
    *nundef = n - *nlocal - *next;
    qsort(tab + *nlocal + *next, *nundef, sizeof(AsmSym *), by_name);
    for (i = 0; i < a->nsyms; i++) {
        index[i] = -1;
    }
    for (i = 0; i < n; i++) {
        index[tab[i] - a->syms] = (int)i;
    }
    return tab;
}

/*
 * The relocations of one section as Mach-O wants them: a local label
 * becomes its atom plus an addend, an addend on a PAGE21/PAGEOFF12/
 * BRANCH26 becomes an ADDEND entry written before the relocation, and an
 * UNSIGNED's addend goes into the bytes. The list comes out in
 * descending address order.
 */
static int macho_relocs(As *a, AsmSection *s, const int *index, MSec *m) {
    size_t i;
    m->rels = malloc((2 * s->nrelocs + 1) * sizeof(MachoRel));
    m->nrels = 0;
    if (!m->rels) {
        fatal("out of memory");
    }
    for (i = s->nrelocs; i-- > 0;) {
        const AsmReloc *r = &s->relocs[i];
        int sym = r->sym;
        long addend = r->addend;
        unsigned long type;
        unsigned long pcrel;
        unsigned long length;
        MachoRel *out;
        if (a->syms[sym].local) {
            int atom = a->syms[sym].atom;
            if (a->syms[sym].section < 0) {
                as_error(a, "undefined label %s", a->syms[sym].name);
                return -1;
            }
            addend += (long)a->syms[sym].off - (long)a->syms[atom].off;
            sym = atom;
        }
        switch (r->kind) {
            case R_ABS64: type = ARM64_RELOC_UNSIGNED; pcrel = 0; length = 3; break;
            case R_BRANCH26: type = ARM64_RELOC_BRANCH26; pcrel = 1; length = 2; break;
            case R_PAGE21: type = ARM64_RELOC_PAGE21; pcrel = 1; length = 2; break;
            case R_PAGEOFF12: type = ARM64_RELOC_PAGEOFF12; pcrel = 0; length = 2; break;
            case R_GOT_PAGE21: type = ARM64_RELOC_GOT_LOAD_PAGE21; pcrel = 1; length = 2; break;
            case R_GOT_PAGEOFF12: type = ARM64_RELOC_GOT_LOAD_PAGEOFF12; pcrel = 0; length = 2; break;
            default:
                as_error(a, "relocation kind %d is not for Mach-O", r->kind);
                return -1;
        }
        if (type == ARM64_RELOC_UNSIGNED) {
            unsigned long v = (unsigned long)addend;
            size_t k;
            for (k = 0; k < 8; k++) {
                s->out[r->off + k] = (unsigned char)(v >> (8 * k));
            }
        } else if (addend != 0) {
            if (addend < -(1l << 23) || addend >= (1l << 23)) {
                as_error(a, "addend %ld is too large for a relocation", addend);
                return -1;
            }
            out = &m->rels[m->nrels++];
            out->addr = r->off;
            out->info = ((unsigned long)addend & 0xffffff) | (2ul << 25) | ((unsigned long)ARM64_RELOC_ADDEND << 28);
        }
        out = &m->rels[m->nrels++];
        out->addr = r->off;
        out->info = (unsigned long)index[sym] | pcrel << 24 | length << 25 | 1ul << 27 | type << 28;
    }
    return 0;
}

int macho_write(As *a, char **out, size_t *out_len) {
    MSec secs[SEC_COUNT];
    int nsects = 0;
    int i;
    int sec;
    unsigned long addr;
    unsigned long hdr_size;
    unsigned long filesize;
    unsigned long reloff;
    unsigned long relstart;
    unsigned long symoff;
    unsigned long stroff;
    unsigned long strsize;
    size_t nlocal;
    size_t next;
    size_t nundef;
    size_t nsyms;
    size_t k;
    int *index;
    AsmSym **tab;
    MachoBuf b;
    int rc = -1;

    memset(secs, 0, sizeof(secs));
    index = malloc((a->nsyms + 1) * sizeof(int));
    if (!index) {
        fatal("out of memory");
    }
    tab = macho_symtab(a, index, &nlocal, &next, &nundef);
    nsyms = nlocal + next + nundef;

    /* the sections, in the order the unit entered them */
    for (sec = 0; sec < SEC_COUNT; sec++) {
        MSec *m;
        if (!a->sec[sec].used) {
            continue;
        }
        m = &secs[nsects++];
        m->kind = sec;
        m->addr = (unsigned long)a->sec[sec].order;    /* sorted on, then overwritten */
        switch (sec) {
            case SEC_TEXT: m->sectname = "__text"; m->segname = "__TEXT"; m->flags = S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS; break;
            case SEC_DATA: m->sectname = "__data"; m->segname = "__DATA"; m->flags = 0; break;
            case SEC_BSS: m->sectname = "__bss"; m->segname = "__DATA"; m->flags = S_ZEROFILL; break;
            case SEC_RODATA: m->sectname = "__const"; m->segname = "__TEXT"; m->flags = 0; break;
            default:
                as_error(a, "a section Mach-O has no name for");
                goto done;
        }
    }
    qsort(secs, (size_t)nsects, sizeof(MSec), macho_by_order);

    /* addresses: the sections with contents in order, then the zero-fill ones */
    addr = 0;
    for (i = 0; i < nsects; i++) {
        AsmSection *s = &a->sec[secs[i].kind];
        unsigned long align = 1ul << s->p2align;
        if (secs[i].kind == SEC_BSS) {
            continue;
        }
        addr = (addr + align - 1) & ~(align - 1);
        secs[i].addr = addr;
        addr += s->size;
    }
    filesize = addr;
    for (i = 0; i < nsects; i++) {
        AsmSection *s = &a->sec[secs[i].kind];
        unsigned long align = 1ul << s->p2align;
        if (secs[i].kind != SEC_BSS) {
            continue;
        }
        addr = (addr + align - 1) & ~(align - 1);
        secs[i].addr = addr;
        addr += s->size;
    }

    /* the file: header and load commands, section contents, relocations, symbols, strings */
    hdr_size = 32 + (72 + 80 * (unsigned long)nsects) + 24 + 24 + 80;
    relstart = hdr_size + ((filesize + 7) & ~7ul);   /* clang pads the contents to 8 before the relocations */
    reloff = relstart;
    for (i = 0; i < nsects; i++) {
        AsmSection *s = &a->sec[secs[i].kind];
        secs[i].fileoff = secs[i].kind == SEC_BSS ? 0 : hdr_size + secs[i].addr;
        if (macho_relocs(a, s, index, &secs[i]) != 0) {
            goto done;
        }
        secs[i].reloff = secs[i].nrels ? reloff : 0;
        reloff += 8 * secs[i].nrels;
    }
    symoff = reloff;
    stroff = symoff + 16 * nsyms;
    strsize = 1;
    for (k = 0; k < nsyms; k++) {
        strsize += strlen(tab[k]->name) + 1;
    }
    strsize = (strsize + 7) & ~7ul;

    memset(&b, 0, sizeof(b));
    put32(&b, MH_MAGIC_64);
    put32(&b, CPU_TYPE_ARM64);
    put32(&b, 0);                       /* cpusubtype: all */
    put32(&b, MH_OBJECT);
    put32(&b, 4);                       /* ncmds */
    put32(&b, hdr_size - 32);           /* sizeofcmds */
    put32(&b, 0);                       /* flags */
    put32(&b, 0);                       /* reserved */

    put32(&b, LC_SEGMENT_64);
    put32(&b, 72 + 80 * (unsigned long)nsects);
    put_name(&b, "");
    put64(&b, 0);                       /* vmaddr */
    put64(&b, addr);                    /* vmsize */
    put64(&b, hdr_size);                /* fileoff */
    put64(&b, filesize);
    put32(&b, 7);                       /* maxprot rwx */
    put32(&b, 7);                       /* initprot */
    put32(&b, (unsigned long)nsects);
    put32(&b, 0);                       /* flags */
    for (i = 0; i < nsects; i++) {
        AsmSection *s = &a->sec[secs[i].kind];
        put_name(&b, secs[i].sectname);
        put_name(&b, secs[i].segname);
        put64(&b, secs[i].addr);
        put64(&b, s->size);
        put32(&b, secs[i].fileoff);
        put32(&b, (unsigned long)s->p2align);
        put32(&b, secs[i].reloff);
        put32(&b, (unsigned long)secs[i].nrels);
        put32(&b, secs[i].flags);
        put32(&b, 0);                   /* reserved1 */
        put32(&b, 0);                   /* reserved2 */
        put32(&b, 0);                   /* reserved3 */
    }

    put32(&b, LC_BUILD_VERSION);
    put32(&b, 24);
    put32(&b, PLATFORM_MACOS);
    put32(&b, 11ul << 16);              /* minos 11.0 */
    put32(&b, 0);                       /* sdk: none */
    put32(&b, 0);                       /* ntools */

    put32(&b, LC_SYMTAB);
    put32(&b, 24);
    put32(&b, symoff);
    put32(&b, (unsigned long)nsyms);
    put32(&b, stroff);
    put32(&b, strsize);

    put32(&b, LC_DYSYMTAB);
    put32(&b, 80);
    put32(&b, 0);                       /* ilocalsym */
    put32(&b, (unsigned long)nlocal);
    put32(&b, (unsigned long)nlocal);   /* iextdefsym */
    put32(&b, (unsigned long)next);
    put32(&b, (unsigned long)(nlocal + next));  /* iundefsym */
    put32(&b, (unsigned long)nundef);
    macho_put_zero(&b, 12 * 4);               /* toc, modtab, extref, indirect, extrel, locrel */

    /* contents */
    for (i = 0; i < nsects; i++) {
        AsmSection *s = &a->sec[secs[i].kind];
        if (secs[i].kind == SEC_BSS) {
            continue;
        }
        macho_put_zero(&b, secs[i].fileoff - b.len);
        if (s->size) {
            macho_put(&b, s->out, s->size);
        }
    }
    macho_put_zero(&b, relstart - b.len);
    for (i = 0; i < nsects; i++) {
        for (k = 0; k < secs[i].nrels; k++) {
            put32(&b, secs[i].rels[k].addr);
            put32(&b, secs[i].rels[k].info);
        }
    }

    /* symbols */
    {
        unsigned long strx = 1;
        for (k = 0; k < nsyms; k++) {
            const AsmSym *s = tab[k];
            put32(&b, strx);
            if (s->section < 0) {
                macho_put(&b, "\1\0\0\0", 4);         /* N_UNDF | N_EXT, no section, n_desc 0 */
                put64(&b, 0);
            } else {
                unsigned char t[4];
                int n;
                for (n = 0; n < nsects && secs[n].kind != s->section; n++) {
                }
                t[0] = (unsigned char)(N_SECT | (s->global ? N_EXT : 0));
                t[1] = (unsigned char)(n + 1);
                t[2] = 0;
                t[3] = 0;
                macho_put(&b, t, 4);
                put64(&b, secs[n].addr + s->off);
            }
            strx += strlen(s->name) + 1;
        }
        macho_put_zero(&b, 1);
        for (k = 0; k < nsyms; k++) {
            macho_put(&b, tab[k]->name, strlen(tab[k]->name) + 1);
        }
        macho_put_zero(&b, stroff + strsize - b.len);
    }

    *out = (char *)b.p;
    *out_len = b.len;
    rc = 0;
done:
    for (i = 0; i < nsects; i++) {
        free(secs[i].rels);
    }
    free(tab);
    free(index);
    return rc;
}
