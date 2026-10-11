/*
 * The ELF64 relocatable object writer (x86_64). The layout follows what
 * clang's integrated assembler writes for the same input so that the two
 * objects can be compared: the sections in the order the unit first
 * entered them, each followed by its .rela section when it has
 * relocations, the symbol table last; the symbols in order of first
 * mention, local ones first, a section's STT_SECTION symbol taking the
 * place of the section's first entry and present only when a relocation
 * refers to it; a relocation against a local symbol going through its
 * section symbol plus the offset, except @PLT and @GOTPCREL ones, which
 * keep the symbol. Differences from clang, on purpose: no .eh_frame (kcc
 * emits no CFI beyond .cfi_startproc/.cfi_endproc) and the string table
 * is not tail-merged, so st_name and sh_name indexes differ.
 */
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "diag.h"

#define EM_X86_64 62
#define ET_REL 1
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHF_WRITE 1
#define SHF_ALLOC 2
#define SHF_EXECINSTR 4
#define SHF_INFO_LINK 0x40
#define STB_GLOBAL 1
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_SECTION 3

enum {
    R_X86_64_64 = 1,
    R_X86_64_PC32 = 2,
    R_X86_64_PLT32 = 4,
    R_X86_64_GOTPCRELX = 41,
    R_X86_64_REX_GOTPCRELX = 42
};

typedef struct ElfBuf {
    unsigned char *p;
    size_t len;
    size_t cap;
} ElfBuf;

static void elf_grow(ElfBuf *b, size_t n) {
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2;
        b->p = realloc(b->p, b->cap);
        if (!b->p) {
            fatal("out of memory");
        }
    }
}

static void elf_put(ElfBuf *b, const void *p, size_t n) {
    elf_grow(b, n);
    memcpy(b->p + b->len, p, n);
    b->len += n;
}

static void elf_put_zero(ElfBuf *b, size_t n) {
    elf_grow(b, n);
    memset(b->p + b->len, 0, n);
    b->len += n;
}

static void put_int(ElfBuf *b, unsigned long v, int bytes) {
    unsigned char x[8];
    int i;
    for (i = 0; i < bytes; i++) {
        x[i] = (unsigned char)(v >> (8 * i));
    }
    elf_put(b, x, (size_t)bytes);
}

static void align(ElfBuf *b, size_t n) {
    elf_put_zero(b, ((b->len + n - 1) & ~(n - 1)) - b->len);
}

/* a relocation entry as written */
typedef struct ElfRel {
    unsigned long off;
    int sym;                /* index in a->syms; the table index comes later */
    unsigned long type;
    long addend;
} ElfRel;

/* a section header to write */
typedef struct Shdr {
    unsigned long name;     /* the string table offset */
    unsigned long type;
    unsigned long flags;
    unsigned long off;
    unsigned long size;
    unsigned long link;
    unsigned long info;
    unsigned long addralign;
    unsigned long entsize;
} Shdr;

/* one of the unit's sections as placed in the object */
typedef struct ESec {
    int kind;               /* SecKind */
    int index;              /* its section header index */
    int rela;               /* the header index of its .rela section, or 0 */
    ElfRel *rels;
    size_t nrels;
} ESec;

static int elf_by_order(const void *x, const void *y) {
    const ESec *a = x;
    const ESec *b = y;
    return a->index - b->index;
}

/*
 * The relocations of one section as ELF wants them: a local symbol is
 * replaced by its section's symbol plus its offset, except under PLT32
 * and GOTPCRELX, which keep the symbol. `used` marks every symbol a
 * relocation ends up naming (the section symbols are written only then).
 */
static int elf_relocs(As *a, AsmSection *s, const int *secsym, ESec *m, char *used) {
    size_t i;
    m->rels = malloc((s->nrelocs + 1) * sizeof(ElfRel));
    m->nrels = 0;
    if (!m->rels) {
        fatal("out of memory");
    }
    for (i = 0; i < s->nrelocs; i++) {
        const AsmReloc *r = &s->relocs[i];
        const AsmSym *sym = &a->syms[r->sym];
        ElfRel *out = &m->rels[m->nrels++];
        out->off = r->off;
        out->sym = r->sym;
        out->addend = r->addend;
        switch (r->kind) {
            case R_ABS64: out->type = R_X86_64_64; break;
            case R_PC32: out->type = R_X86_64_PC32; break;
            case R_PLT32: out->type = R_X86_64_PLT32; break;
            case R_GOTPCRELX: out->type = R_X86_64_GOTPCRELX; break;
            case R_REX_GOTPCRELX: out->type = R_X86_64_REX_GOTPCRELX; break;
            default:
                as_error(a, "relocation kind %d is not for ELF", r->kind);
                return -1;
        }
        if (r->kind == R_ABS64) {
            memset(s->out + r->off, 0, 8);     /* RELA: the addend is in the entry, not the bytes */
        }
        if (sym->section < 0) {
            if (sym->local) {
                as_error(a, "undefined label %s", sym->name);
                return -1;
            }
        } else if (!sym->global && !sym->is_section && (r->kind == R_ABS64 || r->kind == R_PC32)) {
            out->sym = secsym[sym->section];
            out->addend += (long)sym->off;
        }
        used[out->sym] = 1;
    }
    return 0;
}

int elf_write(As *a, char **out, size_t *out_len) {
    ESec secs[SEC_COUNT];
    Shdr hdrs[2 + 2 * SEC_COUNT + 1];
    int nsecs = 0;
    int nhdrs;
    int secsym[SEC_COUNT];
    int *index;                     /* symbol -> symbol table index, or -1 */
    char *used;
    size_t nsyms;
    size_t nlocal;
    size_t i;
    int k;
    int sec;
    int symtab_index;
    unsigned long shoff;
    ElfBuf b;
    ElfBuf strtab;
    int rc = -1;

    memset(secs, 0, sizeof(secs));
    memset(hdrs, 0, sizeof(hdrs));
    memset(&b, 0, sizeof(b));
    memset(&strtab, 0, sizeof(strtab));
    index = malloc((a->nsyms + 1) * sizeof(int));
    used = malloc(a->nsyms + 1);
    if (!index || !used) {
        fatal("out of memory");
    }
    memset(used, 0, a->nsyms + 1);
    elf_put_zero(&strtab, 1);

    /* the section symbols: defined at each section's start when it was first entered */
    for (sec = 0; sec < SEC_COUNT; sec++) {
        secsym[sec] = -1;
    }
    for (i = 0; i < a->nsyms; i++) {
        if (a->syms[i].is_section) {
            secsym[a->syms[i].section] = (int)i;
        }
    }

    /* the sections in order of first entry, each with its .rela after it; headers 0 and 1 are null and .strtab */
    for (sec = 0; sec < SEC_COUNT; sec++) {
        if (a->sec[sec].used) {
            secs[nsecs].kind = sec;
            secs[nsecs].index = a->sec[sec].order;
            nsecs++;
        }
    }
    qsort(secs, (size_t)nsecs, sizeof(ESec), elf_by_order);
    hdrs[1].name = strtab.len;
    elf_put(&strtab, ".strtab", 8);
    hdrs[1].type = SHT_STRTAB;
    hdrs[1].addralign = 1;
    nhdrs = 2;
    for (k = 0; k < nsecs; k++) {
        ESec *m = &secs[k];
        AsmSection *s = &a->sec[m->kind];
        Shdr *h = &hdrs[nhdrs];
        const char *name = "";
        m->index = nhdrs++;
        switch (m->kind) {
            case SEC_TEXT: name = ".text"; h->flags = SHF_ALLOC | SHF_EXECINSTR; break;
            case SEC_DATA: name = ".data"; h->flags = SHF_ALLOC | SHF_WRITE; break;
            case SEC_BSS: name = ".bss"; h->flags = SHF_ALLOC | SHF_WRITE; break;
            case SEC_RODATA: name = ".rodata"; h->flags = SHF_ALLOC; break;
            case SEC_NOTE: name = ".note.GNU-stack"; h->flags = 0; break;
        }
        h->name = strtab.len;
        elf_put(&strtab, name, strlen(name) + 1);
        h->type = m->kind == SEC_BSS ? SHT_NOBITS : SHT_PROGBITS;
        h->size = s->size;
        h->addralign = 1ul << s->p2align;
        if (m->kind == SEC_TEXT && h->addralign < 4) {
            h->addralign = 4;
        }
        if (elf_relocs(a, s, secsym, m, used) != 0) {
            goto done;
        }
        if (m->nrels) {
            Shdr *r = &hdrs[nhdrs];
            m->rela = nhdrs++;
            r->name = strtab.len;
            elf_put(&strtab, ".rela", 5);
            elf_put(&strtab, name, strlen(name) + 1);
            r->type = SHT_RELA;
            r->flags = SHF_INFO_LINK;
            r->size = 24 * m->nrels;
            r->info = (unsigned long)m->index;
            r->addralign = 8;
            r->entsize = 24;
        }
    }
    symtab_index = nhdrs++;
    hdrs[symtab_index].name = strtab.len;
    elf_put(&strtab, ".symtab", 8);
    hdrs[symtab_index].type = SHT_SYMTAB;
    hdrs[symtab_index].link = 1;
    hdrs[symtab_index].addralign = 8;
    hdrs[symtab_index].entsize = 24;
    for (k = 2; k < nhdrs; k++) {
        if (hdrs[k].type == SHT_RELA) {
            hdrs[k].link = (unsigned long)symtab_index;
        }
    }

    /*
     * The symbol table: the null entry, then the locals and the globals,
     * each group in order of first mention. Section symbols go in only
     * when a relocation names them, assembler-local labels never.
     */
    nsyms = 1;
    for (i = 0; i < a->nsyms; i++) {
        index[i] = -1;
    }
    for (k = 0; k < 2; k++) {
        for (i = 0; i < a->nsyms; i++) {
            const AsmSym *s = &a->syms[i];
            int local = s->section >= 0 && !s->global;
            if (s->local || (s->is_section && !used[i]) || (k == 0) != local) {
                continue;
            }
            index[i] = (int)nsyms++;
        }
        if (k == 0) {
            nlocal = nsyms;
        }
    }
    hdrs[symtab_index].size = 24 * nsyms;
    hdrs[symtab_index].info = nlocal;

    /* the file: header, section contents, symbols, relocations, strings, section headers */
    elf_put_zero(&b, 64);
    for (k = 0; k < nsecs; k++) {
        ESec *m = &secs[k];
        AsmSection *s = &a->sec[m->kind];
        Shdr *h = &hdrs[m->index];
        align(&b, h->addralign);
        h->off = b.len;
        if (m->kind != SEC_BSS && s->size) {
            elf_put(&b, s->out, s->size);
        }
    }
    align(&b, 8);
    hdrs[symtab_index].off = b.len;
    elf_put_zero(&b, 24);
    for (k = 0; k < 2; k++) {
        for (i = 0; i < a->nsyms; i++) {
            const AsmSym *s = &a->syms[i];
            unsigned type;
            unsigned shndx = 0;
            int n;
            if (index[i] < 0 || (k == 0) != (s->section >= 0 && !s->global)) {
                continue;
            }
            if (s->is_section) {
                put_int(&b, 0, 4);
                type = STT_SECTION;
            } else {
                put_int(&b, strtab.len, 4);
                elf_put(&strtab, s->name, strlen(s->name) + 1);
                type = s->is_func ? STT_FUNC : s->is_obj ? STT_OBJECT : 0;
            }
            put_int(&b, (unsigned long)((k ? STB_GLOBAL : 0) << 4 | type), 1);
            put_int(&b, 0, 1);              /* st_other */
            if (s->section >= 0) {
                for (n = 0; n < nsecs && secs[n].kind != s->section; n++) {
                }
                shndx = (unsigned)secs[n].index;
            }
            put_int(&b, shndx, 2);
            put_int(&b, s->section >= 0 ? s->off : 0, 8);
            put_int(&b, s->size, 8);
        }
    }
    for (k = 0; k < nsecs; k++) {
        ESec *m = &secs[k];
        if (!m->nrels) {
            continue;
        }
        align(&b, 8);
        hdrs[m->rela].off = b.len;
        for (i = 0; i < m->nrels; i++) {
            const ElfRel *r = &m->rels[i];
            put_int(&b, r->off, 8);
            put_int(&b, r->type, 4);
            put_int(&b, (unsigned long)index[r->sym], 4);
            put_int(&b, (unsigned long)r->addend, 8);
        }
    }
    hdrs[1].off = b.len;
    hdrs[1].size = strtab.len;
    elf_put(&b, strtab.p, strtab.len);
    align(&b, 8);
    shoff = b.len;
    for (k = 0; k < nhdrs; k++) {
        const Shdr *h = &hdrs[k];
        put_int(&b, h->name, 4);
        put_int(&b, h->type, 4);
        put_int(&b, h->flags, 8);
        put_int(&b, 0, 8);                  /* sh_addr */
        put_int(&b, h->off, 8);
        put_int(&b, h->size, 8);
        put_int(&b, h->link, 4);
        put_int(&b, h->info, 4);
        put_int(&b, h->addralign, 8);
        put_int(&b, h->entsize, 8);
    }

    /* the ELF header, now that the section header table has a place */
    {
        unsigned char *h = b.p;
        static const unsigned char ident[16] = { 0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        memcpy(h, ident, 16);
        h[16] = ET_REL;
        h[18] = EM_X86_64;
        h[20] = 1;                          /* e_version */
        for (i = 0; i < 8; i++) {
            h[40 + i] = (unsigned char)(shoff >> (8 * i));
        }
        h[52] = 64;                         /* e_ehsize */
        h[58] = 64;                         /* e_shentsize */
        h[60] = (unsigned char)nhdrs;
        h[61] = (unsigned char)(nhdrs >> 8);
        h[62] = 1;                          /* e_shstrndx: .strtab */
    }

    *out = (char *)b.p;
    *out_len = b.len;
    b.p = NULL;
    rc = 0;
done:
    for (k = 0; k < nsecs; k++) {
        free(secs[k].rels);
    }
    free(b.p);
    free(strtab.p);
    free(index);
    free(used);
    return rc;
}
