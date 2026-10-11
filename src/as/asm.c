/*
 * The assembler's front half: lines into tokens, labels, directives and
 * instructions into sections of items (src/as/asm.h). What an instruction's
 * operands mean is the target's business (arm64.c, x86_64.c); this file
 * only hands it the tokens after the mnemonic and looks the result up in
 * the target's table of accepted (mnemonic, operand shape) rows.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "diag.h"
#include "target.h"
#include "util.h"

#define MAX_ERRORS 20

/* ------------------------------------------------------------------ */
/* diagnostics and memory                                              */
/* ------------------------------------------------------------------ */

void as_error(As *a, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    Loc loc;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    a->errors++;
    if (a->errors > MAX_ERRORS) {
        return;
    }
    loc.file = a->name;
    loc.line = a->line;
    loc.col = 0;
    if (a->errors == MAX_ERRORS) {
        diag_error(loc, "assembler: %s (further errors not reported)", msg);
    } else {
        diag_error(loc, "assembler: %s", msg);
    }
}

/* ------------------------------------------------------------------ */
/* symbols                                                             */
/* ------------------------------------------------------------------ */

static unsigned long as_hash(const char *s, size_t len) {
    unsigned long h = 5381;
    size_t i;
    for (i = 0; i < len; i++) {
        h = h * 33 + (unsigned char)s[i];
    }
    return h;
}

static void rehash(As *a, size_t nbuckets) {
    size_t i;
    a->buckets = xrealloc(a->buckets, nbuckets * sizeof(int));
    a->nbuckets = nbuckets;
    for (i = 0; i < nbuckets; i++) {
        a->buckets[i] = -1;
    }
    for (i = 0; i < a->nsyms; i++) {
        size_t b = as_hash(a->syms[i].name, strlen(a->syms[i].name)) % nbuckets;
        while (a->buckets[b] != -1) {
            b = (b + 1) % nbuckets;
        }
        a->buckets[b] = (int)i;
    }
}

int as_symbol(As *a, const char *s, size_t len) {
    size_t b;
    AsmSym *sym;
    if (a->nsyms * 2 >= a->nbuckets) {
        rehash(a, a->nbuckets ? a->nbuckets * 2 : 1024);
    }
    b = as_hash(s, len) % a->nbuckets;
    while (a->buckets[b] != -1) {
        const char *name = a->syms[a->buckets[b]].name;
        if (strlen(name) == len && memcmp(name, s, len) == 0) {
            return a->buckets[b];
        }
        b = (b + 1) % a->nbuckets;
    }
    if (a->nsyms == a->syms_cap) {
        a->syms_cap = a->syms_cap ? a->syms_cap * 2 : 256;
        a->syms = xrealloc(a->syms, a->syms_cap * sizeof(AsmSym));
    }
    sym = &a->syms[a->nsyms];
    sym->name = xrealloc(NULL, len + 1);
    memcpy(sym->name, s, len);
    sym->name[len] = '\0';
    sym->section = -1;
    sym->item = 0;
    sym->global = 0;
    sym->local = a->arch->local_prefix_dot ? (len > 1 && s[0] == '.' && s[1] == 'L') : (len > 0 && s[0] == 'L');
    sym->is_func = 0;
    sym->sized = 0;
    sym->is_obj = 0;
    sym->is_section = 0;
    sym->off = 0;
    sym->size = 0;
    sym->atom = -1;
    a->buckets[b] = (int)a->nsyms;
    return (int)a->nsyms++;
}

/* ------------------------------------------------------------------ */
/* sections and items                                                  */
/* ------------------------------------------------------------------ */

static AsmItem *add_item(As *a, int kind) {
    AsmSection *s = &a->sec[a->cur];
    AsmItem *it;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->items = xrealloc(s->items, s->cap * sizeof(AsmItem));
    }
    it = &s->items[s->n++];
    memset(it, 0, sizeof(*it));
    it->kind = (unsigned char)kind;
    it->line = a->line;
    s->used = 1;
    return it;
}

/* append bytes to the current section, extending the last ITEM_BYTES when it is the last item */
static void add_bytes(As *a, const unsigned char *p, size_t n) {
    AsmSection *s = &a->sec[a->cur];
    AsmItem *it;
    if (a->cur == SEC_BSS) {
        size_t i;
        for (i = 0; i < n; i++) {
            if (p[i]) {
                as_error(a, "non-zero data in .bss");
                return;
            }
        }
    }
    if (s->data_len + n > s->data_cap) {
        s->data_cap = (s->data_len + n) * 2;
        s->data = xrealloc(s->data, s->data_cap);
    }
    memcpy(s->data + s->data_len, p, n);
    if (s->n > 0 && s->items[s->n - 1].kind == ITEM_BYTES) {
        it = &s->items[s->n - 1];
    } else {
        it = add_item(a, ITEM_BYTES);
        it->u.bytes.off = s->data_len;
    }
    it->u.bytes.len += n;
    s->data_len += n;
}

static void add_int(As *a, unsigned long v, size_t size) {
    unsigned char b[8];
    size_t i;
    for (i = 0; i < size; i++) {
        b[i] = (unsigned char)(v >> (8 * i));
    }
    add_bytes(a, b, size);
}

/* ------------------------------------------------------------------ */
/* the tokenizer                                                       */
/* ------------------------------------------------------------------ */

static int as_is_ident_start(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '.';
}

static int is_ident(int c) {
    return as_is_ident_start(c) || (c >= '0' && c <= '9');
}

int as_tok_is(const AsmTok *t, const char *s) {
    size_t n = strlen(s);
    return t->kind == ATOK_IDENT && t->len == n && memcmp(t->s, s, n) == 0;
}

/*
 * Cut one line (p to end, no newline) into tokens; `strbuf` receives the
 * decoded bytes of string tokens. Returns the count, or -1 after an error.
 */
static int tokenize(As *a, const char *p, const char *end, AsmTok *toks, char *strbuf, size_t strcap) {
    int n = 0;
    size_t used = 0;
    while (p < end) {
        AsmTok *t;
        if (*p == ' ' || *p == '\t') {
            p++;
            continue;
        }
        if (n == MAX_TOKS) {
            as_error(a, "too many tokens on one line");
            return -1;
        }
        t = &toks[n++];
        t->s = NULL;
        t->len = 0;
        t->num = 0;
        t->punct = 0;
        if (as_is_ident_start((unsigned char)*p)) {
            t->kind = ATOK_IDENT;
            t->s = p;
            while (p < end && is_ident((unsigned char)*p)) {
                p++;
            }
            t->len = (size_t)(p - t->s);
        } else if (*p >= '0' && *p <= '9') {
            unsigned long v = 0;
            t->kind = ATOK_NUM;
            if (p + 1 < end && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                p += 2;
                while (p < end) {
                    int c = *p;
                    int d;
                    if (c >= '0' && c <= '9') {
                        d = c - '0';
                    } else if (c >= 'a' && c <= 'f') {
                        d = c - 'a' + 10;
                    } else if (c >= 'A' && c <= 'F') {
                        d = c - 'A' + 10;
                    } else {
                        break;
                    }
                    v = v * 16 + (unsigned long)d;
                    p++;
                }
            } else {
                while (p < end && *p >= '0' && *p <= '9') {
                    v = v * 10 + (unsigned long)(*p - '0');
                    p++;
                }
                if (p + 1 < end && *p == '.' && p[1] >= '0' && p[1] <= '9') {
                    /* a fraction: digits / 10^k, both exact, so one correctly rounded division (no locale, no strtod) */
                    double scale = 1;
                    double val;
                    unsigned long frac = 0;
                    int k = 0;
                    p++;
                    while (p < end && *p >= '0' && *p <= '9') {
                        if (k < 15) {
                            frac = frac * 10 + (unsigned long)(*p - '0');
                            scale *= 10;
                            k++;
                        }
                        p++;
                    }
                    val = (double)v + (double)frac / scale;
                    t->kind = ATOK_FNUM;
                    memcpy(&v, &val, sizeof(v));
                }
            }
            if (p < end && is_ident((unsigned char)*p)) {
                as_error(a, "bad number");
                return -1;
            }
            t->num = v;
        } else if (*p == '"') {
            t->kind = ATOK_STRING;
            t->s = strbuf + used;
            p++;
            while (p < end && *p != '"') {
                int c = (unsigned char)*p++;
                if (c == '\\') {
                    if (p >= end) {
                        break;
                    }
                    c = (unsigned char)*p++;
                    if (c >= '0' && c <= '7') {
                        int k = 1;
                        int v = c - '0';
                        while (k < 3 && p < end && *p >= '0' && *p <= '7') {
                            v = v * 8 + (*p++ - '0');
                            k++;
                        }
                        c = v & 0xff;
                    } else if (c == 'n') {
                        c = '\n';
                    } else if (c == 't') {
                        c = '\t';
                    } else if (c != '"' && c != '\\') {
                        as_error(a, "unknown escape \\%c in string", c);
                        return -1;
                    }
                }
                if (used == strcap) {
                    as_error(a, "string too long");
                    return -1;
                }
                strbuf[used++] = (char)c;
            }
            if (p >= end) {
                as_error(a, "unterminated string");
                return -1;
            }
            p++;
            t->len = (size_t)(strbuf + used - t->s);
        } else if (strchr(",[]!#$%()*+-:@", *p)) {
            t->kind = ATOK_PUNCT;
            t->punct = *p++;
        } else {
            as_error(a, "unexpected character '%c'", *p);
            return -1;
        }
    }
    toks[n].kind = ATOK_END;
    toks[n].s = NULL;
    toks[n].len = 0;
    toks[n].num = 0;
    toks[n].punct = 0;
    return n;
}

/* ------------------------------------------------------------------ */
/* directives                                                          */
/* ------------------------------------------------------------------ */

/* an optionally negated number at t[*i]; -1 if there is none */
static int parse_int(const AsmTok *t, size_t *i, unsigned long *v) {
    int neg = 0;
    if (t[*i].kind == ATOK_PUNCT && t[*i].punct == '-') {
        neg = 1;
        (*i)++;
    }
    if (t[*i].kind != ATOK_NUM) {
        return -1;
    }
    *v = neg ? 0ul - t[*i].num : t[*i].num;
    (*i)++;
    return 0;
}

static int define_symbol(As *a, const char *s, size_t len);

/* enter a section; the first entry gives it its place in the object and its symbol (asm.h, SECSYM_...) */
static int set_section(As *a, int sec) {
    static const char *const names[SEC_COUNT] = { ".text", ".data", ".bss", ".rodata", ".note.GNU-stack" };
    a->cur = sec;
    if (!a->sec[sec].used) {
        a->sec[sec].used = 1;
        a->sec[sec].order = a->nused++;
        if (a->arch->secsym == SECSYM_LTMP) {
            char name[16];
            snprintf(name, sizeof(name), "ltmp%d", a->sec[sec].order);
            define_symbol(a, name, strlen(name));
        } else if (a->arch->secsym == SECSYM_SECTION) {
            int sym = as_symbol(a, names[sec], strlen(names[sec]));
            a->syms[sym].is_section = 1;
            define_symbol(a, names[sec], strlen(names[sec]));
        }
    }
    return 0;
}

/* `.directive args`; t[0] is the directive */
static int directive(As *a, const AsmTok *t, size_t n) {
    const AsmTok *d = &t[0];
    size_t i = 1;
    unsigned long v;

    if (as_tok_is(d, ".text")) {
        if (n != 1) goto junk;
        return set_section(a, SEC_TEXT);
    }
    if (as_tok_is(d, ".data")) {
        if (n != 1) goto junk;
        return set_section(a, SEC_DATA);
    }
    if (as_tok_is(d, ".bss")) {
        if (n != 1) goto junk;
        return set_section(a, SEC_BSS);
    }
    if (as_tok_is(d, ".section")) {
        /* .section __TEXT,__const | .section .rodata | .section .note.GNU-stack,"",@progbits */
        if (n == 4 && as_tok_is(&t[1], "__TEXT") && t[2].kind == ATOK_PUNCT && t[2].punct == ',' && as_tok_is(&t[3], "__const")) {
            return set_section(a, SEC_RODATA);
        }
        if (n == 2 && as_tok_is(&t[1], ".rodata")) {
            return set_section(a, SEC_RODATA);
        }
        if (n == 9 && as_tok_is(&t[1], ".note.GNU") && t[2].punct == '-' && as_tok_is(&t[3], "stack") && t[4].punct == ','
                && t[5].kind == ATOK_STRING && t[5].len == 0 && t[6].punct == ',' && t[7].punct == '@' && as_tok_is(&t[8], "progbits")) {
            return set_section(a, SEC_NOTE);
        }
        as_error(a, "unsupported section");
        return -1;
    }
    if (as_tok_is(d, ".globl")) {
        int sym;
        if (n != 2 || t[1].kind != ATOK_IDENT) goto junk;
        sym = as_symbol(a, t[1].s, t[1].len);     /* may move a->syms: not inside the subscript */
        a->syms[sym].global = 1;
        return 0;
    }
    if (as_tok_is(d, ".p2align")) {
        AsmItem *it;
        if (n != 2 || t[1].kind != ATOK_NUM || t[1].num > 12) goto junk;
        it = add_item(a, ITEM_ALIGN);
        it->u.p2 = (int)t[1].num;
        if (it->u.p2 > a->sec[a->cur].p2align) {
            a->sec[a->cur].p2align = it->u.p2;
        }
        return 0;
    }
    if (as_tok_is(d, ".byte") || as_tok_is(d, ".short") || as_tok_is(d, ".long") || as_tok_is(d, ".quad")) {
        size_t size = as_tok_is(d, ".byte") ? 1 : as_tok_is(d, ".short") ? 2 : as_tok_is(d, ".long") ? 4 : 8;
        if (size == 8 && t[1].kind == ATOK_IDENT) {
            /* .quad sym+addend */
            AsmItem *it;
            int sym = as_symbol(a, t[1].s, t[1].len);
            long addend = 0;
            i = 2;
            if (i < n) {
                int neg;
                if (t[i].kind != ATOK_PUNCT || (t[i].punct != '+' && t[i].punct != '-') || t[i + 1].kind != ATOK_NUM) goto junk;
                neg = t[i].punct == '-';
                addend = (long)t[i + 1].num;
                if (neg) {
                    addend = -addend;
                }
                i += 2;
            }
            if (i != n) goto junk;
            if (a->cur == SEC_BSS) {
                as_error(a, "an address in .bss");
                return -1;
            }
            it = add_item(a, ITEM_ADDR);
            it->u.addr.sym = sym;
            it->u.addr.addend = addend;
            return 0;
        }
        if (size == 4 && n == 4 && t[1].kind == ATOK_IDENT && t[2].kind == ATOK_PUNCT && t[2].punct == '-'
                && t[3].kind == ATOK_IDENT) {
            /* .long a-b: a jump table's entry, the distance between two labels of this section */
            AsmItem *it = add_item(a, ITEM_DIFF);
            it->u.diff.a = as_symbol(a, t[1].s, t[1].len);
            it->u.diff.b = as_symbol(a, t[3].s, t[3].len);
            return 0;
        }
        if (parse_int(t, &i, &v) != 0 || i != n) goto junk;
        add_int(a, v, size);
        return 0;
    }
    if (as_tok_is(d, ".ascii")) {
        if (n != 2 || t[1].kind != ATOK_STRING) goto junk;
        add_bytes(a, (const unsigned char *)t[1].s, t[1].len);
        return 0;
    }
    if (as_tok_is(d, ".space") || as_tok_is(d, ".zero")) {
        static const unsigned char zeros[256];
        if (n != 2 || t[1].kind != ATOK_NUM) goto junk;
        v = t[1].num;
        while (v > 0) {
            size_t k = v > sizeof(zeros) ? sizeof(zeros) : (size_t)v;
            add_bytes(a, zeros, k);
            v -= k;
        }
        return 0;
    }
    if (as_tok_is(d, ".type")) {
        /* .type sym, @function | @object */
        if (n != 5 || t[1].kind != ATOK_IDENT || t[2].punct != ',' || t[3].punct != '@') goto junk;
        if (as_tok_is(&t[4], "function")) {
            int sym = as_symbol(a, t[1].s, t[1].len);
            a->syms[sym].is_func = 1;
        } else if (as_tok_is(&t[4], "object")) {
            int sym = as_symbol(a, t[1].s, t[1].len);
            a->syms[sym].is_obj = 1;
        } else {
            goto junk;
        }
        return 0;
    }
    if (as_tok_is(d, ".size")) {
        /* .size sym, .-sym */
        AsmItem *it;
        int sym;
        if (n != 6 || t[1].kind != ATOK_IDENT || t[2].punct != ',' || !as_tok_is(&t[3], ".") || t[4].punct != '-'
                || t[5].kind != ATOK_IDENT || t[5].len != t[1].len || memcmp(t[1].s, t[5].s, t[1].len) != 0) goto junk;
        sym = as_symbol(a, t[1].s, t[1].len);
        a->syms[sym].sized = 1;
        it = add_item(a, ITEM_SIZE);
        it->u.sym = sym;
        return 0;
    }
    if (as_tok_is(d, ".cfi_startproc") || as_tok_is(d, ".cfi_endproc")) {
        if (n != 1) goto junk;
        return 0;   /* no unwind tables are written */
    }
    as_error(a, "unsupported directive %.*s", (int)d->len, d->s);
    return -1;
junk:
    as_error(a, "bad operands for %.*s", (int)d->len, d->s);
    return -1;
}

/* ------------------------------------------------------------------ */
/* instructions                                                        */
/* ------------------------------------------------------------------ */

static int row_cmp(const char *mnem, const char *shape, const AsmRow *r) {
    int c = strcmp(mnem, r->mnem);
    return c ? c : strcmp(shape, r->shape);
}

/* the table row for (mnem, shape), or -1 */
static int find_row(const AsmArch *arch, const char *mnem, const char *shape) {
    size_t lo = 0;
    size_t hi = arch->nrows;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int c = row_cmp(mnem, shape, &arch->rows[mid]);
        if (c == 0) {
            return (int)mid;
        }
        if (c < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return -1;
}

/* the table must be sorted for the search above: checked once per target */
static void check_table(const AsmArch *arch) {
    static const AsmArch *checked[4];
    size_t i;
    for (i = 0; i < 4; i++) {
        if (checked[i] == arch) {
            return;
        }
        if (!checked[i]) {
            checked[i] = arch;
            break;
        }
    }
    for (i = 1; i < arch->nrows; i++) {
        if (row_cmp(arch->rows[i - 1].mnem, arch->rows[i - 1].shape, &arch->rows[i]) >= 0) {
            fatal("assembler: the %s table is out of order at %s %s", arch->name, arch->rows[i].mnem, arch->rows[i].shape);
        }
    }
}

static int instruction(As *a, const AsmTok *t, size_t n, const char *line, size_t line_len) {
    char mnem[32];
    char shape[MAX_OPS + 1];
    AsmInst insn;
    AsmItem *it;
    size_t k = 1;
    int row;

    if (t[0].len >= sizeof(mnem)) {
        as_error(a, "unknown instruction `%.*s`", (int)line_len, line);
        return -1;
    }
    memcpy(mnem, t[0].s, t[0].len);
    mnem[t[0].len] = '\0';
    if (strcmp(mnem, "rep") == 0 && n >= 2 && t[1].kind == ATOK_IDENT && t[1].len < 16) {
        /* `rep movsb`: one instruction */
        strcat(mnem, " ");
        strncat(mnem, t[1].s, t[1].len);
        k = 2;
    }
    memset(&insn, 0, sizeof(insn));
    if (a->arch->parse(a, mnem, t + k, n - k, &insn) != 0) {
        return -1;
    }
    a->arch->shape(&insn, shape);
    row = find_row(a->arch, mnem, shape);
    if (row < 0) {
        as_error(a, "unsupported instruction form `%.*s` (%s %s)", (int)line_len, line, mnem, shape);
        return -1;
    }
    insn.row = (short)row;
    if (a->cur != SEC_TEXT) {
        as_error(a, "an instruction outside .text");
        return -1;
    }
    it = add_item(a, ITEM_INSN);
    it->u.insn = insn;
    return 0;
}

/* ------------------------------------------------------------------ */
/* the unit                                                            */
/* ------------------------------------------------------------------ */

static void as_init(As *a, const char *name) {
    memset(a, 0, sizeof(*a));
    a->arch = target->as;
    a->name = name;
    check_table(a->arch);
    set_section(a, SEC_TEXT);
}

static void as_free(As *a) {
    size_t i;
    for (i = 0; i < SEC_COUNT; i++) {
        free(a->sec[i].items);
        free(a->sec[i].data);
        free(a->sec[i].out);
        free(a->sec[i].relocs);
    }
    for (i = 0; i < a->nsyms; i++) {
        free(a->syms[i].name);
    }
    free(a->syms);
    free(a->buckets);
}

static int define_symbol(As *a, const char *s, size_t len) {
    int sym = as_symbol(a, s, len);
    AsmItem *it;
    if (a->syms[sym].section >= 0) {
        as_error(a, "label %.*s defined twice", (int)len, s);
        return -1;
    }
    it = add_item(a, ITEM_LABEL);
    it->u.sym = sym;
    a->syms[sym].section = a->cur;
    a->syms[sym].item = a->sec[a->cur].n - 1;
    return 0;
}

static int define_label(As *a, const AsmTok *t) {
    return define_symbol(a, t->s, t->len);
}

/* parse the whole text into `a`; the error count says how it went */
static void parse_unit(As *a, const char *text, size_t len) {
    const char *p = text;
    const char *end = text + len;
    AsmTok toks[MAX_TOKS + 1];
    char *strbuf = NULL;
    size_t strcap = 0;

    a->line = 0;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *line = p;
        size_t line_len;
        int n;
        if (!eol) {
            eol = end;
        }
        line_len = (size_t)(eol - line);
        a->line++;
        p = eol + 1;
        if (line_len + 1 > strcap) {
            strcap = line_len + 1;
            strbuf = xrealloc(strbuf, strcap);
        }
        n = tokenize(a, line, eol, toks, strbuf, strcap);
        if (n < 0) {
            continue;
        }
        if (n == 0) {
            continue;
        }
        if (toks[0].kind == ATOK_IDENT && n == 2 && toks[1].kind == ATOK_PUNCT && toks[1].punct == ':') {
            define_label(a, &toks[0]);
        } else if (toks[0].kind == ATOK_IDENT && toks[0].s[0] == '.' && toks[0].len > 1) {
            directive(a, toks, (size_t)n);
        } else if (toks[0].kind == ATOK_IDENT) {
            instruction(a, toks, (size_t)n, line, line_len);
        } else {
            as_error(a, "cannot parse `%.*s`", (int)line_len, line);
        }
        if (a->errors > MAX_ERRORS) {
            break;
        }
    }
    free(strbuf);
}

int as_check(const char *name, const char *text, size_t len) {
    As a;
    int rc;
    as_init(&a, name);
    parse_unit(&a, text, len);
    rc = a.errors ? -1 : 0;
    as_free(&a);
    return rc;
}

/* ------------------------------------------------------------------ */
/* layout and encoding                                                 */
/* ------------------------------------------------------------------ */

void as_reloc(As *a, int sec, size_t off, int sym, long addend, int kind) {
    AsmSection *s = &a->sec[sec];
    AsmReloc *r;
    if (s->nrelocs == s->relocs_cap) {
        s->relocs_cap = s->relocs_cap ? s->relocs_cap * 2 : 64;
        s->relocs = xrealloc(s->relocs, s->relocs_cap * sizeof(AsmReloc));
    }
    r = &s->relocs[s->nrelocs++];
    r->off = off;
    r->sym = sym;
    r->addend = addend;
    r->kind = (unsigned char)kind;
}

/*
 * Give every item and label its offset. Instruction lengths may depend
 * on label distances (x86_64 jumps), so the pass repeats until they
 * settle; lengths only grow (a jump once widened stays so), so it ends,
 * and at the smallest layout that fits, as clang's relaxation does.
 */
static void as_layout(As *a) {
    int changed = 1;
    int sec;
    while (changed) {
        changed = 0;
        for (sec = 0; sec < SEC_COUNT; sec++) {
            AsmSection *s = &a->sec[sec];
            size_t off = 0;
            size_t i;
            int atom = -1;
            /*
             * Sizes first, all from the offsets of the last pass (a jump's
             * own and its target's from one consistent layout, so that no
             * distance is overestimated), then the offsets for the next.
             */
            for (i = 0; i < s->n; i++) {
                AsmItem *it = &s->items[i];
                if (it->kind == ITEM_INSN) {
                    size_t size = a->arch->insn_size(a, sec, it);
                    if (size != it->size) {
                        it->size = size;
                        changed = 1;
                    }
                }
            }
            for (i = 0; i < s->n; i++) {
                AsmItem *it = &s->items[i];
                it->off = off;
                switch (it->kind) {
                    case ITEM_BYTES:
                        off += it->u.bytes.len;
                        break;
                    case ITEM_INSN:
                        off += it->size;
                        break;
                    case ITEM_LABEL: {
                        AsmSym *sym = &a->syms[it->u.sym];
                        sym->off = off;
                        if (!sym->local) {
                            atom = it->u.sym;
                        }
                        sym->atom = atom;
                        break;
                    }
                    case ITEM_ALIGN: {
                        size_t align = (size_t)1 << it->u.p2;
                        off = (off + align - 1) & ~(align - 1);
                        break;
                    }
                    case ITEM_ADDR:
                        off += 8;
                        break;
                    case ITEM_DIFF:
                        off += 4;
                        break;
                    case ITEM_SIZE: {
                        AsmSym *sym = &a->syms[it->u.sym];
                        if (sym->section == sec) {
                            sym->size = off - sym->off;
                        }
                        break;
                    }
                }
            }
            s->size = off;
        }
    }
}

/* the bytes of every section but .bss, with their relocations */
static void as_emit(As *a) {
    int sec;
    for (sec = 0; sec < SEC_COUNT; sec++) {
        AsmSection *s = &a->sec[sec];
        size_t i;
        if (!s->used || sec == SEC_BSS || s->size == 0) {
            continue;
        }
        s->out = xrealloc(NULL, s->size);
        memset(s->out, 0, s->size);
        for (i = 0; i < s->n; i++) {
            AsmItem *it = &s->items[i];
            a->line = it->line;
            switch (it->kind) {
                case ITEM_BYTES:
                    memcpy(s->out + it->off, s->data + it->u.bytes.off, it->u.bytes.len);
                    break;
                case ITEM_INSN:
                    a->arch->encode(a, sec, it, s->out + it->off);
                    break;
                case ITEM_ALIGN: {
                    size_t end = i + 1 < s->n ? s->items[i + 1].off : s->size;
                    if (sec == SEC_TEXT && end > it->off) {
                        a->arch->pad(s->out + it->off, end - it->off);
                    }
                    break;
                }
                case ITEM_ADDR: {
                    unsigned long v = (unsigned long)it->u.addr.addend;
                    size_t k;
                    if (a->syms[it->u.addr.sym].local && a->syms[it->u.addr.sym].section < 0) {
                        as_error(a, "undefined label %s", a->syms[it->u.addr.sym].name);
                        break;
                    }
                    for (k = 0; k < 8; k++) {
                        s->out[it->off + k] = (unsigned char)(v >> (8 * k));
                    }
                    as_reloc(a, sec, it->off, it->u.addr.sym, it->u.addr.addend, R_ABS64);
                    break;
                }
                case ITEM_DIFF: {
                    const AsmSym *x = &a->syms[it->u.diff.a];
                    const AsmSym *y = &a->syms[it->u.diff.b];
                    unsigned long v;
                    size_t k;
                    if (x->section != sec || y->section != sec) {
                        as_error(a, "the labels of .long %s-%s are not both in this section", x->name, y->name);
                        break;
                    }
                    v = (unsigned long)((long)x->off - (long)y->off);
                    for (k = 0; k < 4; k++) {
                        s->out[it->off + k] = (unsigned char)(v >> (8 * k));
                    }
                    break;
                }
                case ITEM_LABEL:
                case ITEM_SIZE:
                    break;
            }
        }
    }
}

int as_can_write(void) {
    return target->as->write != NULL;
}

int as_assemble(const char *name, const char *text, size_t len, char **out, size_t *out_len) {
    As a;
    int rc = -1;
    *out = NULL;
    *out_len = 0;
    as_init(&a, name);
    parse_unit(&a, text, len);
    if (a.errors == 0 && !a.arch->write) {
        as_error(&a, "no object writer for %s yet", a.arch->name);
    }
    if (a.errors == 0) {
        as_layout(&a);
        as_emit(&a);
    }
    if (a.errors == 0) {
        rc = a.arch->write(&a, out, out_len);
    }
    as_free(&a);
    return rc;
}
