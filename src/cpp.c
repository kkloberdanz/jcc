/*
 * The preprocessor. See cpp.h for what it supports.
 *
 * Structure:
 *   - Sources: the include stack. Each source is a file in memory with a
 *     cursor; line splices and comments are handled as characters are
 *     read, so the token lexer never sees them.
 *   - Tokens: preprocessing tokens in singly linked lists. A token owns
 *     its spelling; copies are made when a token moves between owners.
 *   - Macros: a hash table of object-like, function-like and builtin
 *     macros. Expansion follows the C standard's rescan rules with the
 *     GCC-style context stack: the expansion of a macro is pushed as a
 *     context and the macro is disabled until that context is consumed;
 *     an identifier that names a disabled macro is painted and is never
 *     expanded again.
 *   - Directives are handled when a `#` is the first token on a line of
 *     the file being read, below the expander. Skipped groups are
 *     scanned line by line without tokenizing.
 *   - The writer emits tokens with `# line "file"` markers whenever the
 *     line or file of the next token does not follow from the previous.
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "target.h"
#include "cpp.h"
#include "diag.h"
#include "util.h"

/* ------------------------------------------------------------------ */
/* memory                                                             */
/* ------------------------------------------------------------------ */

static void *pp_alloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) {
        fatal("out of memory");
    }
    return p;
}

static void *pp_realloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) {
        fatal("out of memory");
    }
    return p;
}

static char *pp_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = pp_alloc(n);
    memcpy(p, s, n);
    return p;
}

static char *pp_strndup(const char *s, size_t n) {
    char *p = pp_alloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* a growable byte buffer for spellings */
typedef struct Buf {
    char *s;
    size_t len;
    size_t cap;
} Buf;

static void buf_reset(Buf *b) {
    b->len = 0;
    if (b->s) {
        b->s[0] = '\0';
    }
}

static void buf_push(Buf *b, int c) {
    if (b->len + 2 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->s = pp_realloc(b->s, b->cap);
    }
    b->s[b->len++] = (char)c;
    b->s[b->len] = '\0';
}

static void buf_puts(Buf *b, const char *s) {
    while (*s) {
        buf_push(b, *s++);
    }
}

static void buf_free(Buf *b) {
    free(b->s);
    b->s = NULL;
    b->len = b->cap = 0;
}

/* ------------------------------------------------------------------ */
/* tokens                                                             */
/* ------------------------------------------------------------------ */

typedef enum TokKind {
    TK_EOF,
    TK_EOL,         /* end of a directive line */
    TK_IDENT,
    TK_NUMBER,      /* a pp-number */
    TK_CHAR,
    TK_STRING,
    TK_PUNCT,
    TK_OTHER,       /* any other character */
    TK_PLACEMARKER  /* an empty macro argument next to ## */
} TokKind;

typedef struct Tok {
    TokKind kind;
    char *text;
    Loc loc;
    unsigned ws : 1;        /* whitespace before it */
    unsigned bol : 1;       /* first token on its line */
    unsigned noexpand : 1;  /* painted: names a macro that was disabled when it was seen */
    unsigned op : 1;        /* a # or ## that is an operator in a macro body */
    struct Tok *next;
} Tok;

typedef struct List {
    Tok *head;
    Tok *tail;
} List;

static Tok *tok_new(TokKind kind, const char *text) {
    Tok *t = pp_alloc(sizeof(*t));
    t->kind = kind;
    t->text = pp_strdup(text);
    return t;
}

static Tok *tok_copy(const Tok *t) {
    Tok *c = pp_alloc(sizeof(*c));
    *c = *t;
    c->text = pp_strdup(t->text);
    c->next = NULL;
    return c;
}

static void tok_free(Tok *t) {
    if (t) {
        free(t->text);
        free(t);
    }
}

static void list_free(List *l) {
    Tok *t = l->head;
    while (t) {
        Tok *next = t->next;
        tok_free(t);
        t = next;
    }
    l->head = l->tail = NULL;
}

static void list_append(List *l, Tok *t) {
    t->next = NULL;
    if (l->tail) {
        l->tail->next = t;
    } else {
        l->head = t;
    }
    l->tail = t;
}

static void list_copy(List *dst, const List *src) {
    const Tok *t;
    for (t = src->head; t; t = t->next) {
        list_append(dst, tok_copy(t));
    }
}

static Tok *list_prev(const List *l, const Tok *t) {
    Tok *p = l->head;
    if (p == t) {
        return NULL;
    }
    while (p && p->next != t) {
        p = p->next;
    }
    return p;
}

static void list_remove_tail(List *l) {
    Tok *prev = list_prev(l, l->tail);
    tok_free(l->tail);
    if (prev) {
        prev->next = NULL;
    } else {
        l->head = NULL;
    }
    l->tail = prev;
}

static int tok_is(const Tok *t, TokKind kind, const char *text) {
    return t && t->kind == kind && strcmp(t->text, text) == 0;
}

static int tok_is_punct(const Tok *t, const char *text) {
    return tok_is(t, TK_PUNCT, text);
}

/* ------------------------------------------------------------------ */
/* macros                                                             */
/* ------------------------------------------------------------------ */

typedef enum Builtin {
    B_NONE,
    B_FILE, B_LINE, B_DATE, B_TIME, B_COUNTER, B_INCLUDE_LEVEL, B_BASE_FILE, B_TIMESTAMP,
    B_PRAGMA,           /* _Pragma("...") */
    B_HAS_INCLUDE, B_HAS_INCLUDE_NEXT,
    B_HAS_QUERY         /* __has_feature and friends, answered from the target's table */
} Builtin;

typedef struct Macro {
    char *name;
    int function_like;
    Builtin builtin;
    char **params;
    size_t nparams;
    int variadic;       /* the last parameter takes the rest of the arguments */
    List body;
    int disabled;       /* being rescanned */
    int is_system;      /* defined in a system header */
    Loc loc;
    struct Macro *next;
} Macro;

#define HASH_SIZE 4096

/* ------------------------------------------------------------------ */
/* sources                                                            */
/* ------------------------------------------------------------------ */

typedef struct Cond {
    int was_true;       /* some group of this conditional has been taken */
    int seen_else;
    Loc loc;
    struct Cond *prev;
} Cond;

/* a definition saved by #pragma push_macro, restored by pop_macro */
typedef struct PushedMacro {
    char *name;
    Macro *macro;           /* NULL: the name was not defined */
    struct PushedMacro *next;
} PushedMacro;

typedef struct Source {
    char *buf;
    size_t len;
    size_t pos;
    size_t line;            /* physical line of pos, from 1 */
    size_t line_start;      /* pos of the start of that line */
    long line_delta;        /* #line: presumed line = physical + delta */
    const char *name;       /* presumed file name, interned */
    char *path;             /* as opened, for __TIMESTAMP__ and quoted includes */
    char *dir;              /* directory of path, for quoted includes */
    int dir_index;          /* search directory it was found in, or -1 */
    int is_system;
    int is_virtual;         /* <built-in> or <command line>: no markers */
    int bol;                /* the next token is the first on its line */
    int in_directive;       /* newline ends the token stream with TK_EOL */
    Cond *conds;
    Tok *lookahead;
    struct Source *prev;
} Source;

/* ------------------------------------------------------------------ */
/* state                                                              */
/* ------------------------------------------------------------------ */

typedef struct Name {
    char *s;
    struct Name *next;
} Name;

static struct {
    const CppTarget *target;
    int strict;
    Macro *table[HASH_SIZE];
    Name *names;
    char **dirs;            /* search directories: user ones, then system ones */
    size_t ndirs;
    size_t nuser;
    Source *src;            /* top of the include stack */
    int depth;
    char **once;            /* canonical paths of files with #pragma once */
    size_t nonce;
    PushedMacro *pushed;    /* #pragma push_macro stack */
    unsigned counter;
    char date[16];
    char time[16];
    const char *base_file;
    Buf buf;
    CppReader read;         /* where source text comes from, before the file system */
    void *read_user;
    /* the writer */
    Buf out;
    const char *out_file;
    long out_line;
    size_t out_col;         /* characters written on the current output line */
    int out_bol;
    int need_marker;
    int pending_flag;       /* 1 after entering an include, 2 after leaving one */
    TokKind prev_kind;
    char prev_text[8];
} P;

static const char *intern(const char *s) {
    Name *n;
    for (n = P.names; n; n = n->next) {
        if (strcmp(n->s, s) == 0) {
            return n->s;
        }
    }
    n = pp_alloc(sizeof(*n));
    n->s = pp_strdup(s);
    n->next = P.names;
    P.names = n;
    return n->s;
}

static unsigned hash(const char *s) {
    unsigned h = 5381;
    while (*s) {
        h = h * 33 + (unsigned char)*s++;
    }
    return h % HASH_SIZE;
}

static Macro *macro_lookup(const char *name) {
    Macro *m;
    for (m = P.table[hash(name)]; m; m = m->next) {
        if (strcmp(m->name, name) == 0) {
            return m;
        }
    }
    return NULL;
}

static void macro_free(Macro *m) {
    size_t i;
    for (i = 0; i < m->nparams; i++) {
        free(m->params[i]);
    }
    free(m->params);
    list_free(&m->body);
    free(m->name);
    free(m);
}

static void macro_remove(const char *name) {
    Macro **pp;
    for (pp = &P.table[hash(name)]; *pp; pp = &(*pp)->next) {
        if (strcmp((*pp)->name, name) == 0) {
            Macro *m = *pp;
            *pp = m->next;
            macro_free(m);
            return;
        }
    }
}

static void macro_insert(Macro *m) {
    unsigned h = hash(m->name);
    m->next = P.table[h];
    P.table[h] = m;
}

static Macro *macro_builtin(const char *name, Builtin b, int function_like) {
    Macro *m = pp_alloc(sizeof(*m));
    m->name = pp_strdup(name);
    m->builtin = b;
    m->function_like = function_like;
    if (function_like) {
        m->params = pp_alloc(sizeof(*m->params));
        m->params[0] = pp_strdup("x");
        m->nparams = 1;
    }
    macro_insert(m);
    return m;
}

/* ------------------------------------------------------------------ */
/* characters                                                         */
/* ------------------------------------------------------------------ */

/* skip backslash-newline at the cursor (whitespace between them is tolerated, as compilers do) */
static void skip_splices(Source *s) {
    while (s->pos < s->len && s->buf[s->pos] == '\\') {
        size_t p = s->pos + 1;
        while (p < s->len && (s->buf[p] == ' ' || s->buf[p] == '\t')) {
            p++;
        }
        if (p < s->len && s->buf[p] == '\r') {
            p++;
        }
        if (p < s->len && s->buf[p] == '\n') {
            s->pos = p + 1;
            s->line++;
            s->line_start = s->pos;
        } else {
            break;
        }
    }
}

/* the character at the cursor, or -1 at the end */
static int cur(Source *s) {
    skip_splices(s);
    return s->pos < s->len ? (unsigned char)s->buf[s->pos] : -1;
}

static int peek_at(Source *s, size_t k) {
    size_t pos = s->pos, line = s->line, line_start = s->line_start;
    size_t i;
    int c;
    for (i = 0; i < k; i++) {
        skip_splices(s);
        if (s->pos < s->len) {
            s->pos++;
        }
    }
    c = cur(s);
    s->pos = pos;
    s->line = line;
    s->line_start = line_start;
    return c;
}

/* consume the character at the cursor */
static void advance(Source *s) {
    skip_splices(s);
    if (s->pos < s->len) {
        if (s->buf[s->pos] == '\n') {
            s->line++;
            s->line_start = s->pos + 1;
        }
        s->pos++;
    }
}

static Loc here(Source *s) {
    Loc loc;
    loc.file = s->name;
    loc.line = (size_t)((long)s->line + s->line_delta);
    loc.col = s->pos - s->line_start;
    return loc;
}

static void skip_block_comment(Source *s) {
    Loc loc = here(s);
    advance(s);
    advance(s);
    for (;;) {
        int c = cur(s);
        if (c == -1) {
            diag_error(loc, "unterminated comment");
            return;
        }
        advance(s);
        if (c == '*' && cur(s) == '/') {
            advance(s);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* the token lexer                                                    */
/* ------------------------------------------------------------------ */

static const char *const puncts[] = {
    "...", "<<=", ">>=",
    "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
    "*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=", "##",
    "[", "]", "(", ")", "{", "}", ".", "&", "*", "+", "-", "~", "!", "/", "%",
    "<", ">", "^", "|", "?", ":", ";", "=", ",", "#",
    NULL
};

static int is_ident_start(int c) {
    return isalpha(c) || c == '_' || c == '$';
}

static int is_ident_char(int c) {
    return isalnum(c) || c == '_' || c == '$';
}

static Tok *make_tok(Source *s, TokKind kind, const char *text, Loc loc, int ws) {
    Tok *t = tok_new(kind, text);
    t->loc = loc;
    t->ws = (unsigned)ws;
    t->bol = (unsigned)s->bol;
    return t;
}

/* a string or character literal; the prefix (L, u, U, u8), if any, is already in the buffer */
static Tok *lex_quoted(Source *s, int q, Loc loc, int ws) {
    size_t open = s->pos;
    size_t open_line = s->line, open_line_start = s->line_start;
    Buf *b = &P.buf;
    buf_push(b, q);
    advance(s);
    for (;;) {
        int c = cur(s);
        if (c == -1 || c == '\n') {
            if (q == '"') {
                diag_error(loc, "missing terminating '\"' character");
                return make_tok(s, TK_STRING, b->s, loc, ws);
            }
            /* a lone apostrophe is its own token, as in clang */
            diag_warning(loc, "missing terminating ' character");
            s->pos = open;
            s->line = open_line;
            s->line_start = open_line_start;
            advance(s);
            return make_tok(s, TK_OTHER, "'", loc, ws);
        }
        buf_push(b, c);
        advance(s);
        if (c == '\\') {
            c = cur(s);
            if (c != -1 && c != '\n') {
                buf_push(b, c);
                advance(s);
            }
        } else if (c == q) {
            break;
        }
    }
    return make_tok(s, q == '"' ? TK_STRING : TK_CHAR, b->s, loc, ws);
}

static Tok *lex_token(Source *s) {
    Buf *b = &P.buf;
    Loc loc;
    Tok *t;
    int c;
    int ws = 0;

    for (;;) {
        c = cur(s);
        if (c == ' ' || c == '\t' || c == '\f' || c == '\v' || c == '\r') {
            advance(s);
            ws = 1;
        } else if (c == '\n') {
            if (s->in_directive) {
                loc = here(s);
                advance(s);
                s->bol = 1;
                t = make_tok(s, TK_EOL, "", loc, ws);
                return t;
            }
            advance(s);
            s->bol = 1;
            ws = 0;
        } else if (c == '/' && peek_at(s, 1) == '*') {
            skip_block_comment(s);
            ws = 1;
        } else if (c == '/' && peek_at(s, 1) == '/') {
            while ((c = cur(s)) != '\n' && c != -1) {
                advance(s);
            }
            ws = 1;
        } else {
            break;
        }
    }
    loc = here(s);
    if (c == -1) {
        return make_tok(s, s->in_directive ? TK_EOL : TK_EOF, "", loc, ws);
    }
    buf_reset(b);
    if (is_ident_start(c)) {
        while (is_ident_char(c)) {
            buf_push(b, c);
            advance(s);
            c = cur(s);
        }
        if ((c == '"' || c == '\'') &&
            ((b->len == 1 && (b->s[0] == 'L' || b->s[0] == 'u' || b->s[0] == 'U')) ||
             (b->len == 2 && b->s[0] == 'u' && b->s[1] == '8' && c == '"'))) {
            t = lex_quoted(s, c, loc, ws);
        } else {
            t = make_tok(s, TK_IDENT, b->s, loc, ws);
        }
    } else if (isdigit(c) || (c == '.' && isdigit(peek_at(s, 1)))) {
        buf_push(b, c);
        advance(s);
        for (;;) {
            c = cur(s);
            if ((c == 'e' || c == 'E' || c == 'p' || c == 'P') && (peek_at(s, 1) == '+' || peek_at(s, 1) == '-')) {
                buf_push(b, c);
                advance(s);
                buf_push(b, cur(s));
                advance(s);
            } else if (is_ident_char(c) || c == '.') {
                buf_push(b, c);
                advance(s);
            } else {
                break;
            }
        }
        t = make_tok(s, TK_NUMBER, b->s, loc, ws);
    } else if (c == '"' || c == '\'') {
        t = lex_quoted(s, c, loc, ws);
    } else {
        size_t i;
        t = NULL;
        for (i = 0; puncts[i]; i++) {
            const char *p = puncts[i];
            size_t n = strlen(p);
            size_t k;
            for (k = 0; k < n; k++) {
                if (peek_at(s, k) != (unsigned char)p[k]) {
                    break;
                }
            }
            if (k == n) {
                for (k = 0; k < n; k++) {
                    advance(s);
                }
                t = make_tok(s, TK_PUNCT, p, loc, ws);
                break;
            }
        }
        if (!t) {
            buf_push(b, c);
            advance(s);
            t = make_tok(s, TK_OTHER, b->s, loc, ws);
        }
    }
    s->bol = 0;
    return t;
}

/* the rest of a directive line as raw text, trimmed; consumes the newline */
static char *read_raw_line(Source *s) {
    Buf *b = &P.buf;
    int c;
    buf_reset(b);
    while ((c = cur(s)) != '\n' && c != -1) {
        if (c == '/' && peek_at(s, 1) == '*') {
            skip_block_comment(s);
            buf_push(b, ' ');
            continue;
        }
        buf_push(b, c);
        advance(s);
    }
    if (c == '\n') {
        advance(s);
    }
    s->bol = 1;
    while (b->len > 0 && (b->s[b->len - 1] == ' ' || b->s[b->len - 1] == '\t')) {
        b->s[--b->len] = '\0';
    }
    {
        size_t i = 0;
        while (b->s[i] == ' ' || b->s[i] == '\t') {
            i++;
        }
        return pp_strdup(b->s + i);
    }
}

/*
 * Skip the rest of a line in a skipped group, without tokenizing. Block
 * comments may continue onto later lines; a string or character literal
 * is skipped so a comment opener inside it is not seen, but one that is
 * not closed on its line (an apostrophe in prose) is ignored.
 */
static void skip_line_raw(Source *s) {
    for (;;) {
        int c = cur(s);
        if (c == -1) {
            return;
        }
        if (c == '\n') {
            advance(s);
            s->bol = 1;
            return;
        }
        if (c == '/' && peek_at(s, 1) == '*') {
            skip_block_comment(s);
        } else if (c == '/' && peek_at(s, 1) == '/') {
            while ((c = cur(s)) != '\n' && c != -1) {
                advance(s);
            }
        } else if (c == '"' || c == '\'') {
            size_t save_pos = s->pos, save_line = s->line, save_start = s->line_start;
            int q = c;
            advance(s);
            for (;;) {
                c = cur(s);
                if (c == -1 || c == '\n') {
                    s->pos = save_pos;
                    s->line = save_line;
                    s->line_start = save_start;
                    advance(s);
                    break;
                }
                advance(s);
                if (c == '\\') {
                    if (cur(s) != '\n' && cur(s) != -1) {
                        advance(s);
                    }
                } else if (c == q) {
                    break;
                }
            }
        } else {
            advance(s);
        }
    }
}

/* ------------------------------------------------------------------ */
/* files                                                              */
/* ------------------------------------------------------------------ */

static char *read_file(const char *path, size_t *len) {
    FILE *f;
    char *buf = NULL;
    size_t cap = 0, n = 0;
    struct stat st;

    if (P.read) {
        const char *text = P.read(P.read_user, path, &n);
        if (text) {
            buf = pp_alloc(n + 1);
            memcpy(buf, text, n);
            buf[n] = '\0';
            *len = n;
            return buf;
        }
    }
    f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fstat(fileno(f), &st) == 0 && !S_ISREG(st.st_mode)) {
        fclose(f);
        return NULL;
    }
    for (;;) {
        size_t got;
        if (n + 4096 > cap) {
            cap = cap ? cap * 2 : 8192;
            buf = pp_realloc(buf, cap);
        }
        got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0) {
            break;
        }
    }
    fclose(f);
    buf = pp_realloc(buf, n + 1);
    buf[n] = '\0';
    *len = n;
    return buf;
}

static int file_exists(const char *path) {
    struct stat st;
    size_t len;
    if (P.read && P.read(P.read_user, path, &len)) {
        return 1;
    }
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* trigraphs are replaced only in strict mode, where clang does it too */
static void replace_trigraphs(char *buf, size_t *len) {
    size_t r = 0, w = 0;
    while (r < *len) {
        if (buf[r] == '?' && r + 2 < *len && buf[r + 1] == '?') {
            int c = 0;
            switch (buf[r + 2]) {
                case '=': c = '#'; break;
                case '(': c = '['; break;
                case '/': c = '\\'; break;
                case ')': c = ']'; break;
                case '\'': c = '^'; break;
                case '<': c = '{'; break;
                case '!': c = '|'; break;
                case '>': c = '}'; break;
                case '-': c = '~'; break;
                default: break;
            }
            if (c) {
                buf[w++] = (char)c;
                r += 3;
                continue;
            }
        }
        buf[w++] = buf[r++];
    }
    buf[w] = '\0';
    *len = w;
}

static char *dir_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        return pp_strdup(".");
    }
    if (slash == path) {
        return pp_strdup("/");
    }
    return pp_strndup(path, (size_t)(slash - path));
}

static void push_source(char *buf, size_t len, const char *name, const char *path, int dir_index, int is_system) {
    Source *s = pp_alloc(sizeof(*s));
    s->buf = buf;
    s->len = len;
    s->line = 1;
    s->name = intern(name);
    s->path = path ? pp_strdup(path) : NULL;
    s->dir = path ? dir_of(path) : NULL;
    s->dir_index = dir_index;
    s->is_system = is_system;
    s->bol = 1;
    s->prev = P.src;
    P.src = s;
    P.depth++;
    if (P.strict && strstr(buf, "??")) {
        replace_trigraphs(s->buf, &s->len);
    }
}

static void pop_source(void) {
    Source *s = P.src;
    Cond *c = s->conds;
    while (c) {
        Cond *prev = c->prev;
        free(c);
        c = prev;
    }
    tok_free(s->lookahead);
    P.src = s->prev;
    P.depth--;
    if (!s->is_virtual) {
        P.pending_flag = 2;
        P.need_marker = 1;
    }
    free(s->buf);
    free(s->path);
    free(s->dir);
    free(s);
}

/* ------------------------------------------------------------------ */
/* the writer                                                         */
/* ------------------------------------------------------------------ */

static int is_punct_prefix(const char *s) {
    size_t i, n = strlen(s);
    for (i = 0; puncts[i]; i++) {
        if (strncmp(puncts[i], s, n) == 0) {
            return 1;
        }
    }
    return 0;
}

/* would writing `t` right after the previous token change how they lex? */
static int needs_space(const Tok *t) {
    TokKind pk = P.prev_kind;
    int c = (unsigned char)t->text[0];
    if (pk == TK_IDENT || pk == TK_NUMBER) {
        if (t->kind == TK_IDENT || t->kind == TK_NUMBER || t->kind == TK_CHAR || t->kind == TK_STRING) {
            return 1;
        }
        if (pk == TK_NUMBER && (c == '.' || c == '+' || c == '-')) {
            return 1;
        }
    }
    if (pk == TK_PUNCT) {
        char joined[16];
        if (t->kind == TK_NUMBER && strcmp(P.prev_text, ".") == 0) {
            return 1;
        }
        if (strcmp(P.prev_text, "/") == 0 && (c == '*' || c == '/')) {
            return 1;
        }
        if (t->kind == TK_PUNCT) {
            snprintf(joined, sizeof(joined), "%s%c", P.prev_text, c);
            return is_punct_prefix(joined);
        }
    }
    return 0;
}

static void write_marker(const char *file, long line) {
    char marker[32];
    if (!P.out_bol) {
        buf_push(&P.out, '\n');
    }
    snprintf(marker, sizeof(marker), "# %ld ", line);
    buf_puts(&P.out, marker);
    buf_push(&P.out, '"');
    buf_puts(&P.out, file);
    buf_push(&P.out, '"');
    if (P.pending_flag) {
        snprintf(marker, sizeof(marker), " %d", P.pending_flag);
        buf_puts(&P.out, marker);
    }
    if (P.src && P.src->is_system) {
        buf_puts(&P.out, " 3");
    }
    buf_push(&P.out, '\n');
    P.pending_flag = 0;
    P.need_marker = 0;
    P.out_file = file;
    P.out_line = line;
    P.out_bol = 1;
    P.out_col = 0;
    P.prev_kind = TK_EOF;
}

static void emit(const Tok *t) {
    long line = (long)t->loc.line;
    if (P.need_marker || t->loc.file != P.out_file || line < P.out_line || line > P.out_line + 8) {
        if (!P.need_marker && t->loc.file == P.out_file && line == P.out_line + 1 && !P.out_bol) {
            buf_push(&P.out, '\n');
            P.out_line++;
            P.out_bol = 1;
            P.out_col = 0;
        } else {
            write_marker(t->loc.file, line);
        }
    }
    while (P.out_line < line) {
        buf_push(&P.out, '\n');
        P.out_line++;
        P.out_bol = 1;
        P.out_col = 0;
        P.prev_kind = TK_EOF;
    }
    if (P.out_bol) {
        P.out_col = 0;
    }
    /*
     * The first token of a line keeps its source column (the line's
     * indentation), so that a diagnostic's column is the source line's.
     * Later tokens follow with a space where the source had one; after a
     * macro expansion their columns may differ from the source.
     */
    if (P.out_bol && t->loc.file == P.out_file && line == P.out_line) {
        while (P.out_col < t->loc.col) {
            buf_push(&P.out, ' ');
            P.out_col++;
        }
    } else if (!P.out_bol && (t->ws || needs_space(t))) {
        buf_push(&P.out, ' ');
        P.out_col++;
    }
    buf_puts(&P.out, t->text);
    P.out_col += strlen(t->text);
    P.out_bol = 0;
    P.prev_kind = t->kind;
    strncpy(P.prev_text, t->text, sizeof(P.prev_text) - 1);
    P.prev_text[sizeof(P.prev_text) - 1] = '\0';
}

/* a #pragma line goes to the output as it is */
static void emit_pragma(const char *text) {
    if (!P.out_bol) {
        buf_push(&P.out, '\n');
    }
    buf_puts(&P.out, "#pragma ");
    buf_puts(&P.out, text);
    buf_push(&P.out, '\n');
    P.out_bol = 1;
    P.out_col = 0;
    P.need_marker = 1;
}

/* ------------------------------------------------------------------ */
/* reading tokens from the include stack                              */
/* ------------------------------------------------------------------ */

static void handle_directive(Source *s);

/* the next token of the file being read, with directives handled; NULL at the end of everything */
static Tok *read_file_token(void) {
    for (;;) {
        Source *s = P.src;
        Tok *t;
        if (!s) {
            return NULL;
        }
        if (s->lookahead) {
            t = s->lookahead;
            s->lookahead = NULL;
            return t;
        }
        t = lex_token(s);
        if (t->kind == TK_EOF) {
            tok_free(t);
            if (s->conds) {
                diag_error(s->conds->loc, "unterminated conditional directive");
            }
            pop_source();
            continue;
        }
        if (t->kind == TK_EOL) {
            return t;
        }
        if (t->bol && tok_is_punct(t, "#") && !s->in_directive) {
            tok_free(t);
            handle_directive(s);
            continue;
        }
        return t;
    }
}

/* ------------------------------------------------------------------ */
/* the expander                                                       */
/* ------------------------------------------------------------------ */

typedef struct Context {
    Tok *toks;
    Macro *macro;           /* re-enabled when the context is exhausted */
    struct Context *prev;
} Context;

typedef struct Expander {
    Context *ctx;
    int from_file;          /* read the include stack when the contexts run out */
    int pending_ws;         /* a macro expanded to nothing: its leading space goes to the next token */
} Expander;

static void push_context(Expander *x, Tok *toks, Macro *m) {
    Context *c = pp_alloc(sizeof(*c));
    c->toks = toks;
    c->macro = m;
    c->prev = x->ctx;
    x->ctx = c;
    if (m) {
        m->disabled = 1;
    }
}

/* the next token without macro expansion; NULL when there are no more */
static Tok *x_next(Expander *x) {
    for (;;) {
        Context *c = x->ctx;
        Tok *t;
        if (!c) {
            return x->from_file ? read_file_token() : NULL;
        }
        if (c->toks) {
            t = c->toks;
            c->toks = t->next;
            t->next = NULL;
            return t;
        }
        if (c->macro) {
            c->macro->disabled = 0;
        }
        x->ctx = c->prev;
        free(c);
    }
}

static void x_unget(Expander *x, Tok *t) {
    push_context(x, t, NULL);
}

static void x_free(Expander *x) {
    while (x->ctx) {
        Context *c = x->ctx;
        List l;
        l.head = c->toks;
        l.tail = NULL;
        list_free(&l);
        if (c->macro) {
            c->macro->disabled = 0;
        }
        x->ctx = c->prev;
        free(c);
    }
}

static Tok *expand_next(Expander *x);

/* fully expand a token list on its own (an argument), returning a fresh list */
static List expand_list(Tok *toks) {
    Expander x;
    List out;
    Tok *t;
    List copy;
    x.ctx = NULL;
    x.from_file = 0;
    x.pending_ws = 0;
    copy.head = copy.tail = NULL;
    for (t = toks; t; t = t->next) {
        list_append(&copy, tok_copy(t));
    }
    push_context(&x, copy.head, NULL);
    out.head = out.tail = NULL;
    while ((t = expand_next(&x)) != NULL) {
        list_append(&out, t);
    }
    return out;
}

/* the spelling of a token list, with a single space wherever there was whitespace */
static void spell_list(Buf *b, const Tok *toks, int keep_ws) {
    const Tok *t;
    for (t = toks; t; t = t->next) {
        if (t->kind == TK_PLACEMARKER) {
            continue;
        }
        if (keep_ws && t != toks && (t->ws || t->bol)) {
            buf_push(b, ' ');
        }
        buf_puts(b, t->text);
    }
}

/* the # operator */
static Tok *stringify(const Tok *toks, Loc loc) {
    Buf b;
    const Tok *t;
    Tok *r;
    memset(&b, 0, sizeof(b));
    buf_push(&b, '"');
    for (t = toks; t; t = t->next) {
        const char *p;
        if (t->kind == TK_PLACEMARKER) {
            continue;
        }
        if (t != toks && (t->ws || t->bol)) {
            buf_push(&b, ' ');
        }
        for (p = t->text; *p; p++) {
            if ((t->kind == TK_STRING || t->kind == TK_CHAR) && (*p == '"' || *p == '\\')) {
                buf_push(&b, '\\');
            }
            buf_push(&b, *p);
        }
    }
    buf_push(&b, '"');
    r = tok_new(TK_STRING, b.s);
    r->loc = loc;
    buf_free(&b);
    return r;
}

/* the ## operator: the two spellings must lex as exactly one token */
static int paste(Tok *left, const Tok *right, Loc loc) {
    Source s;
    Tok *t;
    int n;
    int ok;
    Buf b;
    if (right->kind == TK_PLACEMARKER) {
        return 1;
    }
    if (left->kind == TK_PLACEMARKER) {
        free(left->text);
        left->text = pp_strdup(right->text);
        left->kind = right->kind;
        return 1;
    }
    memset(&b, 0, sizeof(b));
    buf_puts(&b, left->text);
    buf_puts(&b, right->text);
    memset(&s, 0, sizeof(s));
    s.buf = b.s;
    s.len = b.len;
    s.line = 1;
    s.name = loc.file;
    n = diag_error_count();
    t = lex_token(&s);
    ok = diag_error_count() == n && t->kind != TK_EOF && s.pos == s.len;
    if (ok) {
        free(left->text);
        left->text = t->text;
        left->kind = t->kind;
        t->text = NULL;
    } else {
        diag_error(loc, "pasting \"%s\" and \"%s\" does not give a valid preprocessing token", left->text, right->text);
    }
    tok_free(t);
    buf_free(&b);
    return ok;
}

static int param_index(const Macro *m, const Tok *t) {
    size_t i;
    if (t->kind != TK_IDENT) {
        return -1;
    }
    for (i = 0; i < m->nparams; i++) {
        if (strcmp(m->params[i], t->text) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* substitute the arguments into the body of a function-like macro, or copy an object-like body */
static List substitute(Macro *m, List *args, List *expanded, const Tok *name) {
    List out;
    const Tok *b;
    out.head = out.tail = NULL;
    for (b = m->body.head; b; b = b->next) {
        int idx;
        /* # param */
        if (m->function_like && b->op && tok_is_punct(b, "#") && b->next && (idx = param_index(m, b->next)) >= 0) {
            Tok *t = stringify(args[idx].head, name->loc);
            t->ws = b->ws;
            list_append(&out, t);
            b = b->next;
            continue;
        }
        idx = m->function_like ? param_index(m, b) : -1;
        if (idx >= 0) {
            int raw = (out.tail && out.tail->op && tok_is_punct(out.tail, "##")) ||
                      (b->next && b->next->op && tok_is_punct(b->next, "##"));
            const Tok *a;
            /*
             * GNU: `, ## args` for the variadic parameter. With no variadic
             * arguments the comma goes too; otherwise the ## does nothing.
             */
            if (m->variadic && idx == (int)m->nparams - 1 && out.tail && out.tail->op &&
                tok_is_punct(out.tail, "##") && list_prev(&out, out.tail) &&
                tok_is_punct(list_prev(&out, out.tail), ",")) {
                list_remove_tail(&out);
                if (!args[idx].head) {
                    list_remove_tail(&out);
                    continue;
                }
            }
            if (raw) {
                if (!args[idx].head) {
                    Tok *t = tok_new(TK_PLACEMARKER, "");
                    t->loc = name->loc;
                    t->ws = b->ws;
                    list_append(&out, t);
                }
                for (a = args[idx].head; a; a = a->next) {
                    Tok *t = tok_copy(a);
                    t->loc = name->loc;
                    if (a == args[idx].head) {
                        t->ws = b->ws;
                    } else if (t->bol) {
                        t->ws = 1;
                    }
                    t->bol = 0;
                    list_append(&out, t);
                }
            } else {
                if (!expanded[idx].head && args[idx].head) {
                    expanded[idx] = expand_list(args[idx].head);
                }
                for (a = expanded[idx].head; a; a = a->next) {
                    Tok *t = tok_copy(a);
                    t->loc = name->loc;
                    if (a == expanded[idx].head) {
                        t->ws = b->ws;
                    } else if (t->bol) {
                        t->ws = 1;
                    }
                    t->bol = 0;
                    list_append(&out, t);
                }
            }
            continue;
        }
        {
            Tok *t = tok_copy(b);
            t->loc = name->loc;
            t->bol = 0;
            list_append(&out, t);
        }
    }
    /* ## */
    {
        Tok *prev = NULL, *t = out.head;
        while (t) {
            if (t->op && tok_is_punct(t, "##") && prev && t->next) {
                Tok *right = t->next;
                paste(prev, right, name->loc);
                prev->next = right->next;
                if (out.tail == right) {
                    out.tail = prev;
                }
                tok_free(t);
                tok_free(right);
                t = prev->next;
                continue;
            }
            prev = t;
            t = t->next;
        }
    }
    /* drop placemarkers, and the operator mark on anything that came from the body */
    {
        Tok **pp = &out.head;
        out.tail = NULL;
        while (*pp) {
            Tok *t = *pp;
            if (t->kind == TK_PLACEMARKER) {
                *pp = t->next;
                tok_free(t);
                continue;
            }
            t->op = 0;
            out.tail = t;
            pp = &t->next;
        }
    }
    if (out.head) {
        out.head->ws = name->ws;
        out.head->bol = 0;
    }
    return out;
}

/*
 * Collect the arguments of a function-like macro; the `(` has been read.
 * Returns the number of arguments, or -1 on error. Each argument is a
 * token list; a variadic macro's last argument takes everything after
 * the other parameters, commas included.
 */
static int collect_args(Expander *x, Macro *m, const Tok *name, List *args) {
    int depth = 0;
    size_t n = 0;
    size_t want = m->nparams;
    for (;;) {
        Tok *t = x_next(x);
        if (!t || t->kind == TK_EOL || t->kind == TK_EOF) {
            if (t) {
                if (t->kind == TK_EOL && P.src) {
                    P.src->lookahead = t;
                } else {
                    tok_free(t);
                }
            }
            diag_error(name->loc, "unterminated argument list invoking macro '%s'", m->name);
            return -1;
        }
        if (depth == 0 && tok_is_punct(t, ")")) {
            tok_free(t);
            break;
        }
        if (depth == 0 && tok_is_punct(t, ",") && !(m->variadic && n + 1 >= want)) {
            tok_free(t);
            n++;
            if (n >= want) {
                diag_error(name->loc, "too many arguments provided to function-like macro invocation of '%s'", m->name);
                /* skip to the closing parenthesis */
                for (;;) {
                    t = x_next(x);
                    if (!t || t->kind == TK_EOL || t->kind == TK_EOF) {
                        if (t && t->kind == TK_EOL && P.src) {
                            P.src->lookahead = t;
                        } else {
                            tok_free(t);
                        }
                        return -1;
                    }
                    if (tok_is_punct(t, "(")) {
                        depth++;
                    } else if (tok_is_punct(t, ")")) {
                        if (depth == 0) {
                            tok_free(t);
                            return -1;
                        }
                        depth--;
                    }
                    tok_free(t);
                }
            }
            continue;
        }
        if (tok_is_punct(t, "(")) {
            depth++;
        } else if (tok_is_punct(t, ")")) {
            depth--;
        }
        if (n < want) {
            if (t->bol) {
                t->ws = 1;
                t->bol = 0;
            }
            list_append(&args[n], t);
        } else {
            tok_free(t);
        }
    }
    if (want == 0) {
        return 0;
    }
    n++;
    if (n < want) {
        if (m->variadic && n + 1 == want) {
            /* the variadic part may be left out entirely */
            return (int)want;
        }
        diag_error(name->loc, "too few arguments provided to function-like macro invocation of '%s'", m->name);
        return -1;
    }
    return (int)n;
}

static const char *has_query_answer(const char *query) {
    const CppHasAnswer *a;
    for (a = P.target->has; a && a->query; a++) {
        if (strcmp(a->query, query) == 0) {
            return a->value ? "1" : "0";
        }
    }
    return "0";
}

static int find_include(const char *name, int angle, int next, char **path_out, int *dir_index, int *is_system);

/* the one token (or none) a builtin macro expands to */
static List expand_builtin(Macro *m, const Tok *name, List *args) {
    List out;
    char tmp[64];
    Tok *t = NULL;
    Buf b;
    out.head = out.tail = NULL;
    memset(&b, 0, sizeof(b));
    switch (m->builtin) {
        case B_FILE:
        case B_BASE_FILE: {
            const char *p = m->builtin == B_FILE ? name->loc.file : P.base_file;
            buf_push(&b, '"');
            for (; *p; p++) {
                if (*p == '"' || *p == '\\') {
                    buf_push(&b, '\\');
                }
                buf_push(&b, *p);
            }
            buf_push(&b, '"');
            t = tok_new(TK_STRING, b.s);
            break;
        }
        case B_LINE:
            snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)name->loc.line);
            t = tok_new(TK_NUMBER, tmp);
            break;
        case B_DATE:
            t = tok_new(TK_STRING, P.date);
            break;
        case B_TIME:
            t = tok_new(TK_STRING, P.time);
            break;
        case B_COUNTER:
            snprintf(tmp, sizeof(tmp), "%u", P.counter++);
            t = tok_new(TK_NUMBER, tmp);
            break;
        case B_INCLUDE_LEVEL:
            snprintf(tmp, sizeof(tmp), "%d", P.depth > 0 ? P.depth - 1 : 0);
            t = tok_new(TK_NUMBER, tmp);
            break;
        case B_TIMESTAMP: {
            struct stat st;
            const char *text = "\"??? ??? ?? ??:??:?? ????\"";
            if (P.src && P.src->path && stat(P.src->path, &st) == 0) {
                char *c = ctime(&st.st_mtime);
                if (c) {
                    size_t n = strlen(c);
                    if (n && c[n - 1] == '\n') {
                        c[n - 1] = '\0';
                    }
                    snprintf(tmp, sizeof(tmp), "\"%s\"", c);
                    text = tmp;
                }
            }
            t = tok_new(TK_STRING, text);
            break;
        }
        case B_PRAGMA: {
            /* _Pragma("text"): destringize and emit a #pragma line */
            Tok *a = args[0].head;
            if (!a || a->kind != TK_STRING || a->next) {
                diag_error(name->loc, "_Pragma takes a parenthesized string literal");
                break;
            }
            {
                const char *p = a->text;
                while (*p && *p != '"') {
                    p++;
                }
                for (p++; *p && !(*p == '"' && !p[1]); p++) {
                    if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) {
                        p++;
                    }
                    buf_push(&b, *p);
                }
                if (strcmp(b.s ? b.s : "", "once") == 0) {
                    diag_warning(name->loc, "#pragma once in _Pragma is ignored");
                } else {
                    emit_pragma(b.s ? b.s : "");
                }
            }
            break;
        }
        case B_HAS_INCLUDE:
        case B_HAS_INCLUDE_NEXT: {
            Tok *a = args[0].head;
            List ex;
            int angle = 0;
            ex.head = ex.tail = NULL;
            if (a && a->kind == TK_IDENT) {
                ex = expand_list(a);
                a = ex.head;
            }
            if (a && a->kind == TK_STRING && !a->next) {
                buf_puts(&b, a->text + 1);
                if (b.len && b.s[b.len - 1] == '"') {
                    b.s[--b.len] = '\0';
                }
            } else if (a && tok_is_punct(a, "<")) {
                for (a = a->next; a && !tok_is_punct(a, ">"); a = a->next) {
                    buf_puts(&b, a->text);
                }
                angle = 1;
            } else {
                diag_error(name->loc, "expected \"FILENAME\" or <FILENAME>");
                list_free(&ex);
                break;
            }
            {
                char *path = NULL;
                int di, sys;
                int found = find_include(b.s ? b.s : "", angle, m->builtin == B_HAS_INCLUDE_NEXT, &path, &di, &sys);
                free(path);
                t = tok_new(TK_NUMBER, found ? "1" : "0");
            }
            list_free(&ex);
            break;
        }
        case B_HAS_QUERY:
            buf_puts(&b, m->name);
            buf_push(&b, '(');
            spell_list(&b, args[0].head, 0);
            buf_push(&b, ')');
            t = tok_new(TK_NUMBER, has_query_answer(b.s));
            break;
        case B_NONE:
            break;
    }
    if (t) {
        t->loc = name->loc;
        t->ws = name->ws;
        list_append(&out, t);
    }
    buf_free(&b);
    return out;
}

/* an expansion produced nothing: the space before the macro name moves to whatever follows, as in clang */
static Tok *deliver(Expander *x, Tok *t) {
    if (x->pending_ws && t) {
        t->ws = 1;
        x->pending_ws = 0;
    }
    return t;
}

/* the next fully macro-expanded token; NULL when there are no more */
static Tok *expand_next(Expander *x) {
    for (;;) {
        Tok *t = x_next(x);
        Macro *m;
        if (!t || t->kind != TK_IDENT || t->noexpand) {
            return deliver(x, t);
        }
        m = macro_lookup(t->text);
        if (!m) {
            return deliver(x, t);
        }
        if (m->disabled) {
            t->noexpand = 1;
            return deliver(x, t);
        }
        if (!m->function_like) {
            List r;
            if (m->builtin) {
                r = expand_builtin(m, t, NULL);
            } else {
                List none;
                none.head = none.tail = NULL;
                r = substitute(m, NULL, &none, t);
            }
            if (!r.head && t->ws) {
                x->pending_ws = 1;
            }
            tok_free(t);
            push_context(x, r.head, m->builtin ? NULL : m);
            continue;
        }
        {
            Tok *peek = x_next(x);
            List *args;
            List *expanded;
            List r;
            size_t i;
            int n;
            if (!peek || !tok_is_punct(peek, "(")) {
                if (peek) {
                    x_unget(x, peek);
                }
                return deliver(x, t);
            }
            tok_free(peek);
            args = pp_alloc((m->nparams + 1) * sizeof(*args));
            expanded = pp_alloc((m->nparams + 1) * sizeof(*expanded));
            n = collect_args(x, m, t, args);
            if (n < 0) {
                for (i = 0; i < m->nparams; i++) {
                    list_free(&args[i]);
                }
                free(args);
                free(expanded);
                tok_free(t);
                continue;
            }
            if (m->builtin) {
                r = expand_builtin(m, t, args);
            } else {
                r = substitute(m, args, expanded, t);
            }
            for (i = 0; i < m->nparams; i++) {
                list_free(&args[i]);
                list_free(&expanded[i]);
            }
            free(args);
            free(expanded);
            if (!r.head && t->ws) {
                x->pending_ws = 1;
            }
            tok_free(t);
            push_context(x, r.head, m->builtin ? NULL : m);
            continue;
        }
    }
}

/* ------------------------------------------------------------------ */
/* #if expressions                                                    */
/* ------------------------------------------------------------------ */

typedef struct Val {
    unsigned long v;
    int uns;
} Val;

typedef struct Expr {
    Tok **toks;
    size_t n;
    size_t pos;
    int error;
    int unevaluated;    /* inside the untaken side of && || ?: so division by zero is not an error */
    Loc loc;
} Expr;

static Val mkval(unsigned long v, int uns) {
    Val r;
    r.v = v;
    r.uns = uns;
    return r;
}

static Tok *ex_peek(Expr *e) {
    return e->pos < e->n ? e->toks[e->pos] : NULL;
}

static int ex_accept(Expr *e, const char *punct) {
    Tok *t = ex_peek(e);
    if (t && tok_is_punct(t, punct)) {
        e->pos++;
        return 1;
    }
    return 0;
}

static void ex_error(Expr *e, Loc loc, const char *msg) {
    if (!e->error) {
        diag_error(loc, "%s", msg);
        e->error = 1;
    }
}

static int char_value(const char *text, Loc loc) {
    const char *p = text;
    int wide = 0;
    long v = 0;
    int nchars = 0;
    if (*p == 'L' || *p == 'u' || *p == 'U') {
        wide = 1;
        p++;
    }
    p++;    /* ' */
    while (*p && *p != '\'') {
        int c;
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n': c = '\n'; p++; break;
                case 't': c = '\t'; p++; break;
                case 'r': c = '\r'; p++; break;
                case 'a': c = '\a'; p++; break;
                case 'b': c = '\b'; p++; break;
                case 'f': c = '\f'; p++; break;
                case 'v': c = '\v'; p++; break;
                case 'e': c = 27; p++; break;
                case 'x': {
                    c = 0;
                    p++;
                    while (isxdigit((unsigned char)*p)) {
                        c = c * 16 + (isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
                        p++;
                    }
                    break;
                }
                case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
                    int k = 0;
                    c = 0;
                    while (k < 3 && *p >= '0' && *p <= '7') {
                        c = c * 8 + (*p - '0');
                        p++;
                        k++;
                    }
                    break;
                }
                default: c = (unsigned char)*p; p++; break;
            }
        } else {
            c = (unsigned char)*p++;
        }
        if (!wide) {
            c &= 0xff;
            if (arch_info->char_is_signed && c >= 128) {
                c -= 256;
            }
        }
        v = nchars == 0 ? c : (long)(((unsigned long)v << 8) | (unsigned)(c & 0xff));
        nchars++;
    }
    if (nchars == 0) {
        diag_error(loc, "empty character constant");
    } else if (nchars > 1) {
        diag_warning(loc, "multi-character character constant");
    }
    return (int)v;
}

static Val ex_cond(Expr *e);

static Val ex_primary(Expr *e) {
    Tok *t = ex_peek(e);
    if (!t) {
        ex_error(e, e->loc, "expected value in expression");
        return mkval(0, 0);
    }
    e->pos++;
    if (tok_is_punct(t, "(")) {
        Val v = ex_cond(e);
        if (!ex_accept(e, ")")) {
            ex_error(e, t->loc, "expected ')' in preprocessor expression");
        }
        return v;
    }
    if (t->kind == TK_NUMBER) {
        const char *p = t->text;
        char *end;
        unsigned long v;
        int uns = 0;
        int ll = 0;
        if (strchr(p, '.') || ((p[0] != '0' || (p[1] != 'x' && p[1] != 'X')) && (strchr(p, 'e') || strchr(p, 'E')))) {
            ex_error(e, t->loc, "floating point literal in preprocessor expression");
            return mkval(0, 0);
        }
        v = strtoul(p, &end, 0);
        while (*end) {
            if (*end == 'u' || *end == 'U') {
                uns = 1;
            } else if (*end == 'l' || *end == 'L') {
                ll++;
            } else {
                ex_error(e, t->loc, "invalid suffix on integer constant");
                return mkval(0, 0);
            }
            end++;
        }
        if (!uns && v > 0x7fffffffffffffffUL) {
            if (p[0] == '0') {
                uns = 1;    /* octal and hex constants become unsigned */
            } else {
                diag_warning(t->loc, "integer literal is too large to be represented in a signed integer type, interpreting as unsigned");
                uns = 1;
            }
        }
        UNUSED(ll);
        return mkval(v, uns);
    }
    if (t->kind == TK_CHAR) {
        return mkval((unsigned long)(long)char_value(t->text, t->loc), 0);
    }
    if (t->kind == TK_IDENT) {
        /* an identifier that is not a macro is 0, as the standard says */
        return mkval(0, 0);
    }
    ex_error(e, t->loc, "token is not valid in preprocessor expression");
    return mkval(0, 0);
}

static Val ex_unary(Expr *e) {
    Tok *t = ex_peek(e);
    if (t && t->kind == TK_PUNCT) {
        if (ex_accept(e, "+")) {
            return ex_unary(e);
        }
        if (ex_accept(e, "-")) {
            Val v = ex_unary(e);
            return mkval(0 - v.v, v.uns);
        }
        if (ex_accept(e, "~")) {
            Val v = ex_unary(e);
            return mkval(~v.v, v.uns);
        }
        if (ex_accept(e, "!")) {
            Val v = ex_unary(e);
            return mkval(v.v == 0, 0);
        }
    }
    return ex_primary(e);
}

static Val ex_binary(Expr *e, int level);

static const char *const binops[][5] = {
    { "*", "/", "%", NULL },
    { "+", "-", NULL },
    { "<<", ">>", NULL },
    { "<", ">", "<=", ">=", NULL },
    { "==", "!=", NULL },
    { "&", NULL },
    { "^", NULL },
    { "|", NULL }
};

static Val ex_apply(Expr *e, const char *op, Val a, Val b, Loc loc) {
    int uns = a.uns || b.uns;
    long sa = (long)a.v, sb = (long)b.v;
    if (strcmp(op, "*") == 0) {
        return mkval(uns ? a.v * b.v : (unsigned long)(sa * sb), uns);
    }
    if (strcmp(op, "/") == 0 || strcmp(op, "%") == 0) {
        if (b.v == 0) {
            if (!e->unevaluated) {
                ex_error(e, loc, "division by zero in preprocessor expression");
            }
            return mkval(0, uns);
        }
        if (op[0] == '/') {
            return mkval(uns ? a.v / b.v : (unsigned long)(sa / sb), uns);
        }
        return mkval(uns ? a.v % b.v : (unsigned long)(sa % sb), uns);
    }
    if (strcmp(op, "+") == 0) {
        return mkval(a.v + b.v, uns);
    }
    if (strcmp(op, "-") == 0) {
        return mkval(a.v - b.v, uns);
    }
    if (strcmp(op, "<<") == 0) {
        if (b.v >= 64) {
            return mkval(0, a.uns);
        }
        return mkval(a.v << b.v, a.uns);
    }
    if (strcmp(op, ">>") == 0) {
        if (b.v >= 64) {
            return mkval(a.uns || sa >= 0 ? 0 : ~0UL, a.uns);
        }
        return mkval(a.uns ? a.v >> b.v : (unsigned long)(sa >> b.v), a.uns);
    }
    if (strcmp(op, "<") == 0) {
        return mkval(uns ? a.v < b.v : sa < sb, 0);
    }
    if (strcmp(op, ">") == 0) {
        return mkval(uns ? a.v > b.v : sa > sb, 0);
    }
    if (strcmp(op, "<=") == 0) {
        return mkval(uns ? a.v <= b.v : sa <= sb, 0);
    }
    if (strcmp(op, ">=") == 0) {
        return mkval(uns ? a.v >= b.v : sa >= sb, 0);
    }
    if (strcmp(op, "==") == 0) {
        return mkval(a.v == b.v, 0);
    }
    if (strcmp(op, "!=") == 0) {
        return mkval(a.v != b.v, 0);
    }
    if (strcmp(op, "&") == 0) {
        return mkval(a.v & b.v, uns);
    }
    if (strcmp(op, "^") == 0) {
        return mkval(a.v ^ b.v, uns);
    }
    return mkval(a.v | b.v, uns);
}

static Val ex_binary(Expr *e, int level) {
    Val a;
    if (level < 0) {
        return ex_unary(e);
    }
    a = ex_binary(e, level - 1);
    for (;;) {
        Tok *t = ex_peek(e);
        const char *op = NULL;
        size_t i;
        if (!t || t->kind != TK_PUNCT) {
            return a;
        }
        for (i = 0; binops[level][i]; i++) {
            if (strcmp(binops[level][i], t->text) == 0) {
                op = binops[level][i];
                break;
            }
        }
        if (!op) {
            return a;
        }
        e->pos++;
        a = ex_apply(e, op, a, ex_binary(e, level - 1), t->loc);
    }
}

static Val ex_land(Expr *e) {
    Val a = ex_binary(e, 7);
    while (ex_accept(e, "&&")) {
        Val b;
        int save = e->unevaluated;
        if (a.v == 0) {
            e->unevaluated = 1;
        }
        b = ex_binary(e, 7);
        e->unevaluated = save;
        a = mkval(a.v != 0 && b.v != 0, 0);
    }
    return a;
}

static Val ex_lor(Expr *e) {
    Val a = ex_land(e);
    while (ex_accept(e, "||")) {
        Val b;
        int save = e->unevaluated;
        if (a.v != 0) {
            e->unevaluated = 1;
        }
        b = ex_land(e);
        e->unevaluated = save;
        a = mkval(a.v != 0 || b.v != 0, 0);
    }
    return a;
}

static Val ex_cond(Expr *e) {
    Val c = ex_lor(e);
    if (ex_accept(e, "?")) {
        Val a, b;
        int save = e->unevaluated;
        if (c.v == 0) {
            e->unevaluated = 1;
        }
        a = ex_cond(e);
        e->unevaluated = save;
        if (!ex_accept(e, ":")) {
            ex_error(e, e->loc, "expected ':' in preprocessor expression");
            return mkval(0, 0);
        }
        if (c.v != 0) {
            e->unevaluated = 1;
        }
        b = ex_cond(e);
        e->unevaluated = save;
        return c.v != 0 ? mkval(a.v, a.uns || b.uns) : mkval(b.v, a.uns || b.uns);
    }
    return c;
}

/* read the rest of the directive line through the expander and evaluate it */
static int eval_if(Source *s, Loc loc) {
    Expander x;
    Expr e;
    Tok **arr = NULL;
    size_t n = 0, cap = 0, i;
    Val v;
    int result;

    x.ctx = NULL;
    x.from_file = 1;
    x.pending_ws = 0;
    memset(&e, 0, sizeof(e));
    e.loc = loc;
    for (;;) {
        Tok *t = expand_next(&x);
        if (!t) {
            break;
        }
        if (t->kind == TK_EOL) {
            tok_free(t);
            break;
        }
        if (t->kind == TK_IDENT && strcmp(t->text, "defined") == 0) {
            Tok *a = x_next(&x);
            int paren = 0;
            Loc dloc = t->loc;
            tok_free(t);
            if (a && tok_is_punct(a, "(")) {
                paren = 1;
                tok_free(a);
                a = x_next(&x);
            }
            if (!a || a->kind != TK_IDENT) {
                ex_error(&e, dloc, "macro name missing after 'defined'");
                if (a && a->kind == TK_EOL) {
                    tok_free(a);
                    break;
                }
                tok_free(a);
                continue;
            }
            t = tok_new(TK_NUMBER, macro_lookup(a->text) ? "1" : "0");
            t->loc = a->loc;
            tok_free(a);
            if (paren) {
                a = x_next(&x);
                if (!a || !tok_is_punct(a, ")")) {
                    ex_error(&e, dloc, "expected ')' after 'defined' operand");
                    if (a && a->kind == TK_EOL) {
                        tok_free(a);
                        tok_free(t);
                        break;
                    }
                }
                tok_free(a);
            }
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            arr = pp_realloc(arr, cap * sizeof(*arr));
        }
        arr[n++] = t;
    }
    x_free(&x);
    UNUSED(s);
    e.toks = arr;
    e.n = n;
    if (n == 0) {
        ex_error(&e, loc, "expected value in expression");
    }
    v = ex_cond(&e);
    if (!e.error && e.pos < n) {
        ex_error(&e, arr[e.pos]->loc, "token is not valid in preprocessor expression");
    }
    result = !e.error && v.v != 0;
    for (i = 0; i < n; i++) {
        tok_free(arr[i]);
    }
    free(arr);
    return result;
}

/* ------------------------------------------------------------------ */
/* include search                                                     */
/* ------------------------------------------------------------------ */

static char *join_path(const char *dir, const char *name) {
    size_t n = strlen(dir);
    size_t size = n + strlen(name) + 3;
    char *p = pp_alloc(size);
    if (n == 0 || strcmp(dir, ".") == 0) {
        snprintf(p, size, "%s%s", n ? "./" : "", name);
    } else if (dir[n - 1] == '/') {
        snprintf(p, size, "%s%s", dir, name);
    } else {
        snprintf(p, size, "%s/%s", dir, name);
    }
    return p;
}

/*
 * Find an include file. Quoted names are looked up in the including
 * file's directory first. `next` starts after the directory the current
 * file was found in (#include_next). Returns 1 and the path on success.
 */
static int find_include(const char *name, int angle, int next, char **path_out, int *dir_index, int *is_system) {
    size_t i, start = 0;
    Source *s = P.src;
    if (name[0] == '/') {
        if (file_exists(name)) {
            *path_out = pp_strdup(name);
            *dir_index = -1;
            *is_system = s ? s->is_system : 0;
            return 1;
        }
        return 0;
    }
    if (next) {
        if (s && s->dir_index >= 0) {
            start = (size_t)s->dir_index + 1;
        }
    } else if (!angle && s && s->dir) {
        char *p = join_path(s->dir, name);
        if (file_exists(p)) {
            *path_out = p;
            *dir_index = -1;
            *is_system = s->is_system;
            return 1;
        }
        free(p);
    }
    for (i = start; i < P.ndirs; i++) {
        char *p = join_path(P.dirs[i], name);
        if (file_exists(p)) {
            *path_out = p;
            *dir_index = (int)i;
            *is_system = i >= P.nuser;
            return 1;
        }
        free(p);
    }
    return 0;
}

static int seen_once(const char *path) {
    char real[4096];
    size_t i;
    const char *key = realpath(path, real) ? real : path;
    for (i = 0; i < P.nonce; i++) {
        if (strcmp(P.once[i], key) == 0) {
            return 1;
        }
    }
    return 0;
}

static void mark_once(const char *path) {
    char real[4096];
    const char *key = realpath(path, real) ? real : path;
    if (seen_once(key)) {
        return;
    }
    P.once = pp_realloc(P.once, (P.nonce + 1) * sizeof(*P.once));
    P.once[P.nonce++] = pp_strdup(key);
}

/* ------------------------------------------------------------------ */
/* directives                                                         */
/* ------------------------------------------------------------------ */

/* read to the end of the directive line, complaining about anything there */
static void expect_eol(Source *s, const char *directive) {
    Tok *t = lex_token(s);
    if (t->kind != TK_EOL) {
        diag_warning(t->loc, "extra tokens at end of #%s directive", directive);
        while (t->kind != TK_EOL) {
            tok_free(t);
            t = lex_token(s);
        }
    }
    tok_free(t);
}

static void skip_to_eol(Source *s) {
    free(read_raw_line(s));
}

/* drop a token and the rest of its directive line, unless the token was the end of it */
static void discard_line(Source *s, Tok *t) {
    if (t && t->kind != TK_EOL) {
        skip_to_eol(s);
    }
    tok_free(t);
}

static int is_builtin_name(const char *name) {
    Macro *m = macro_lookup(name);
    return m && m->builtin;
}

static int lists_equal(const Tok *a, const Tok *b) {
    while (a && b) {
        if (a->kind != b->kind || strcmp(a->text, b->text) != 0 || a->ws != b->ws) {
            return 0;
        }
        a = a->next;
        b = b->next;
    }
    return a == b;
}

static int macros_equal(const Macro *a, const Macro *b) {
    size_t i;
    if (a->function_like != b->function_like || a->nparams != b->nparams || a->variadic != b->variadic) {
        return 0;
    }
    for (i = 0; i < a->nparams; i++) {
        if (strcmp(a->params[i], b->params[i]) != 0) {
            return 0;
        }
    }
    return lists_equal(a->body.head, b->body.head);
}

static void do_define(Source *s) {
    Tok *name = lex_token(s);
    Macro *m;
    Tok *t;
    Macro *old;

    if (name->kind != TK_IDENT) {
        diag_error(name->loc, "macro name must be an identifier");
        discard_line(s, name);
        return;
    }
    if (strcmp(name->text, "defined") == 0) {
        diag_error(name->loc, "'defined' cannot be used as a macro name");
        tok_free(name);
        skip_to_eol(s);
        return;
    }
    if (is_builtin_name(name->text)) {
        diag_warning(name->loc, "redefining builtin macro '%s' is ignored", name->text);
        tok_free(name);
        skip_to_eol(s);
        return;
    }
    m = pp_alloc(sizeof(*m));
    m->name = pp_strdup(name->text);
    m->loc = name->loc;
    tok_free(name);

    t = lex_token(s);
    if (tok_is_punct(t, "(") && !t->ws) {
        m->function_like = 1;
        tok_free(t);
        t = lex_token(s);
        if (!tok_is_punct(t, ")")) {
            for (;;) {
                if (tok_is_punct(t, "...")) {
                    m->variadic = 1;
                    m->params = pp_realloc(m->params, (m->nparams + 1) * sizeof(*m->params));
                    m->params[m->nparams++] = pp_strdup("__VA_ARGS__");
                    tok_free(t);
                    t = lex_token(s);
                    if (!tok_is_punct(t, ")")) {
                        diag_error(t->loc, "missing ')' in macro parameter list");
                        goto bad;
                    }
                    break;
                }
                if (t->kind != TK_IDENT) {
                    diag_error(t->loc, "invalid token in macro parameter list");
                    goto bad;
                }
                if (param_index(m, t) >= 0) {
                    diag_error(t->loc, "duplicate macro parameter name '%s'", t->text);
                    goto bad;
                }
                m->params = pp_realloc(m->params, (m->nparams + 1) * sizeof(*m->params));
                m->params[m->nparams++] = pp_strdup(t->text);
                tok_free(t);
                t = lex_token(s);
                if (tok_is_punct(t, "...")) {
                    /* GNU named variadic parameter */
                    m->variadic = 1;
                    tok_free(t);
                    t = lex_token(s);
                    if (!tok_is_punct(t, ")")) {
                        diag_error(t->loc, "missing ')' in macro parameter list");
                        goto bad;
                    }
                    break;
                }
                if (tok_is_punct(t, ")")) {
                    break;
                }
                if (!tok_is_punct(t, ",")) {
                    diag_error(t->loc, "expected comma in macro parameter list");
                    goto bad;
                }
                tok_free(t);
                t = lex_token(s);
            }
        }
        tok_free(t);
        t = lex_token(s);
    }
    /* the body */
    while (t->kind != TK_EOL) {
        t->bol = 0;
        if (tok_is_punct(t, "#") || tok_is_punct(t, "##")) {
            t->op = 1;
        }
        list_append(&m->body, t);
        t = lex_token(s);
    }
    tok_free(t);
    if (m->body.head) {
        m->body.head->ws = 0;
    }
    /* # must name a parameter; ## cannot be at either end */
    for (t = m->body.head; t; t = t->next) {
        if (m->function_like && tok_is_punct(t, "#") && !(t->next && param_index(m, t->next) >= 0)) {
            diag_error(t->loc, "'#' is not followed by a macro parameter");
            goto bad_body;
        }
        if (tok_is_punct(t, "##") && (t == m->body.head || !t->next)) {
            diag_error(t->loc, "'##' cannot appear at either end of a macro expansion");
            goto bad_body;
        }
    }
    old = macro_lookup(m->name);
    m->is_system = P.src && P.src->is_system;
    if (old) {
        /* as clang: no complaint when either definition is a system header's (math.h redefines INFINITY) */
        if (!macros_equal(old, m) && !old->is_system && !m->is_system) {
            diag_warning(m->loc, "'%s' macro redefined", m->name);
            diag_note(old->loc, "previous definition is here");
        }
        macro_remove(m->name);
    }
    macro_insert(m);
    return;

bad:
    discard_line(s, t);
bad_body:
    macro_free(m);
}

static void do_undef(Source *s) {
    Tok *name = lex_token(s);
    if (name->kind != TK_IDENT) {
        diag_error(name->loc, "macro name must be an identifier");
        discard_line(s, name);
        return;
    }
    if (is_builtin_name(name->text)) {
        diag_warning(name->loc, "undefining builtin macro '%s' is ignored", name->text);
    } else {
        macro_remove(name->text);
    }
    tok_free(name);
    expect_eol(s, "undef");
}

/* #include, #include_next and #import */
static void do_include(Source *s, Loc loc, int next, int once) {
    Tok *t = lex_token(s);
    Buf b;
    int angle = 0;
    char *path = NULL;
    int dir_index, is_system;
    char *buf;
    size_t len;

    memset(&b, 0, sizeof(b));
    if (t->kind == TK_STRING && t->text[0] == '"') {
        buf_puts(&b, t->text + 1);
        if (b.len && b.s[b.len - 1] == '"') {
            b.s[--b.len] = '\0';
        }
        tok_free(t);
        expect_eol(s, next ? "include_next" : "include");
    } else if (tok_is_punct(t, "<")) {
        int c;
        angle = 1;
        tok_free(t);
        while ((c = cur(s)) != '>' && c != '\n' && c != -1) {
            buf_push(&b, c);
            advance(s);
        }
        if (c != '>') {
            diag_error(loc, "expected '>' in #include");
            skip_to_eol(s);
            buf_free(&b);
            return;
        }
        advance(s);
        expect_eol(s, next ? "include_next" : "include");
    } else if (t->kind == TK_IDENT) {
        /* a macro that expands to a header name */
        Expander x;
        List toks;
        Tok *e;
        x.ctx = NULL;
        x.from_file = 1;
        x.pending_ws = 0;
        toks.head = toks.tail = NULL;
        if (s->lookahead) {
            tok_free(s->lookahead);
        }
        s->lookahead = t;
        while ((e = expand_next(&x)) != NULL && e->kind != TK_EOL) {
            list_append(&toks, e);
        }
        tok_free(e);
        x_free(&x);
        e = toks.head;
        if (e && e->kind == TK_STRING && e->text[0] == '"' && !e->next) {
            buf_puts(&b, e->text + 1);
            if (b.len && b.s[b.len - 1] == '"') {
                b.s[--b.len] = '\0';
            }
        } else if (e && tok_is_punct(e, "<")) {
            angle = 1;
            for (e = e->next; e && !tok_is_punct(e, ">"); e = e->next) {
                if (e->ws && b.len) {
                    buf_push(&b, ' ');
                }
                buf_puts(&b, e->text);
            }
            if (!e) {
                diag_error(loc, "expected '>' in #include");
                list_free(&toks);
                buf_free(&b);
                return;
            }
        } else {
            diag_error(loc, "expected \"FILENAME\" or <FILENAME>");
            list_free(&toks);
            buf_free(&b);
            return;
        }
        list_free(&toks);
    } else {
        diag_error(t->loc, "expected \"FILENAME\" or <FILENAME>");
        discard_line(s, t);
        buf_free(&b);
        return;
    }
    if (b.len == 0) {
        diag_error(loc, "empty filename");
        buf_free(&b);
        return;
    }
    if (next && (!s->prev || s->dir_index < 0)) {
        diag_warning(loc, "#include_next in file found relative to primary source file or found by absolute path; will search from start of include path");
        next = 0;
    }
    if (!find_include(b.s, angle, next, &path, &dir_index, &is_system)) {
        diag_error(loc, "'%s' file not found", b.s);
        buf_free(&b);
        return;
    }
    buf_free(&b);
    if (seen_once(path)) {
        free(path);
        return;
    }
    if (once) {
        mark_once(path);
    }
    if (P.depth >= 200) {
        diag_error(loc, "#include nested too deeply");
        free(path);
        return;
    }
    buf = read_file(path, &len);
    if (!buf) {
        diag_error(loc, "cannot read '%s'", path);
        free(path);
        return;
    }
    s->in_directive = 0;
    push_source(buf, len, path, path, dir_index, is_system);
    P.pending_flag = 1;
    P.need_marker = 1;
    free(path);
}

/* #line and the GNU `# 33 "file" flags` marker */
static void do_line(Source *s, Tok *first) {
    Tok *t = first;
    unsigned long n;
    char *end;
    if (t->kind != TK_NUMBER) {
        /* #line may use macros */
        Expander x;
        Tok *e;
        x.ctx = NULL;
        x.from_file = 1;
        x.pending_ws = 0;
        if (s->lookahead) {
            tok_free(s->lookahead);
        }
        s->lookahead = t;
        e = expand_next(&x);
        x_free(&x);
        t = e;
    }
    if (!t || t->kind != TK_NUMBER || (n = strtoul(t->text, &end, 10), *end != '\0')) {
        diag_error(t ? t->loc : here(s), "#line directive requires a positive integer argument");
        discard_line(s, t);
        return;
    }
    tok_free(t);
    t = lex_token(s);
    if (t->kind == TK_STRING && t->text[0] == '"') {
        Buf b;
        memset(&b, 0, sizeof(b));
        buf_puts(&b, t->text + 1);
        if (b.len && b.s[b.len - 1] == '"') {
            b.s[--b.len] = '\0';
        }
        s->name = intern(b.s);
        buf_free(&b);
        tok_free(t);
        t = lex_token(s);
    }
    /* flags on a GNU marker are ignored */
    while (t->kind != TK_EOL) {
        tok_free(t);
        t = lex_token(s);
    }
    tok_free(t);
    /* the next physical line is line n */
    s->line_delta = (long)n - (long)s->line;
    P.need_marker = 1;
}

/* #pragma push_macro("NAME") / pop_macro("NAME"): a stack of saved definitions per name */
static void do_push_pop_macro(Source *s, Loc loc, int push) {
    Tok *t = lex_token(s);
    char *name;
    size_t n;
    if (t->kind != TK_PUNCT || strcmp(t->text, "(") != 0) {
        diag_error(loc, "expected '(' after push_macro or pop_macro");
        discard_line(s, t);
        return;
    }
    tok_free(t);
    t = lex_token(s);
    if (t->kind != TK_STRING || t->text[0] != '"') {
        diag_error(loc, "expected a string literal naming the macro");
        discard_line(s, t);
        return;
    }
    n = strlen(t->text);
    name = pp_strndup(t->text + 1, n - 2);
    tok_free(t);
    t = lex_token(s);
    if (t->kind != TK_PUNCT || strcmp(t->text, ")") != 0) {
        diag_error(loc, "expected ')' after the macro name");
        discard_line(s, t);
    } else {
        tok_free(t);
        expect_eol(s, "pragma");
    }
    if (push) {
        Macro *m = macro_lookup(name);
        PushedMacro *pm = pp_alloc(sizeof(*pm));
        pm->name = name;
        pm->macro = NULL;
        if (m) {
            /* a private copy: the live definition may be redefined or undefined */
            Macro *c = pp_alloc(sizeof(*c));
            size_t i;
            *c = *m;
            c->next = NULL;
            c->name = pp_strdup(m->name);
            c->params = m->nparams ? pp_alloc(m->nparams * sizeof(*c->params)) : NULL;
            for (i = 0; i < m->nparams; i++) {
                c->params[i] = pp_strdup(m->params[i]);
            }
            memset(&c->body, 0, sizeof(c->body));
            list_copy(&c->body, &m->body);
            pm->macro = c;
        }
        pm->next = P.pushed;
        P.pushed = pm;
    } else {
        PushedMacro **pp;
        for (pp = &P.pushed; *pp; pp = &(*pp)->next) {
            if (strcmp((*pp)->name, name) == 0) {
                PushedMacro *pm = *pp;
                *pp = pm->next;
                macro_remove(name);
                if (pm->macro) {
                    macro_insert(pm->macro);
                }
                free(pm->name);
                free(pm);
                break;
            }
        }
        free(name);
    }
}

static void do_pragma(Source *s, Loc loc) {
    Tok *t = lex_token(s);
    Buf b;
    if (t->kind == TK_IDENT && strcmp(t->text, "once") == 0) {
        tok_free(t);
        expect_eol(s, "pragma");
        if (s->path) {
            mark_once(s->path);
        } else {
            diag_warning(loc, "#pragma once in main file");
        }
        return;
    }
    if (t->kind == TK_IDENT && (strcmp(t->text, "push_macro") == 0 || strcmp(t->text, "pop_macro") == 0)) {
        int push = t->text[1] == 'u';
        tok_free(t);
        do_push_pop_macro(s, loc, push);
        return;
    }
    memset(&b, 0, sizeof(b));
    while (t->kind != TK_EOL) {
        if (b.len && t->ws) {
            buf_push(&b, ' ');
        }
        buf_puts(&b, t->text);
        tok_free(t);
        t = lex_token(s);
    }
    tok_free(t);
    emit_pragma(b.s ? b.s : "");
    buf_free(&b);
}

static void push_cond(Source *s, int was_true, Loc loc) {
    Cond *c = pp_alloc(sizeof(*c));
    c->was_true = was_true;
    c->loc = loc;
    c->prev = s->conds;
    s->conds = c;
}

static void pop_cond(Source *s) {
    Cond *c = s->conds;
    s->conds = c->prev;
    free(c);
}

/*
 * Skip a group whose condition was false, through the #elif, #else or
 * #endif that ends it. On return the directive that ended the skip has
 * been consumed and, for #elif and #else, its group is to be taken.
 */
static void skip_group(Source *s) {
    int depth = 0;
    for (;;) {
        int c;
        Tok *t;
        for (;;) {
            c = cur(s);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
                advance(s);
            } else if (c == '/' && peek_at(s, 1) == '*') {
                skip_block_comment(s);
            } else {
                break;
            }
        }
        if (c == -1) {
            diag_error(s->conds->loc, "unterminated conditional directive");
            pop_cond(s);
            return;
        }
        if (c == '\n') {
            advance(s);
            s->bol = 1;
            continue;
        }
        if (c != '#') {
            skip_line_raw(s);
            continue;
        }
        advance(s);
        s->in_directive = 1;
        t = lex_token(s);
        if (t->kind == TK_IDENT) {
            const char *d = t->text;
            if (strcmp(d, "if") == 0 || strcmp(d, "ifdef") == 0 || strcmp(d, "ifndef") == 0) {
                depth++;
            } else if (depth == 0) {
                Cond *cond = s->conds;
                if (strcmp(d, "elif") == 0) {
                    Loc loc = t->loc;
                    if (cond->seen_else) {
                        diag_error(loc, "#elif after #else");
                    }
                    if (!cond->was_true) {
                        tok_free(t);
                        if (eval_if(s, loc)) {
                            cond->was_true = 1;
                            s->in_directive = 0;
                            return;
                        }
                        s->in_directive = 0;
                        continue;
                    }
                } else if (strcmp(d, "else") == 0) {
                    if (cond->seen_else) {
                        diag_error(t->loc, "#else after #else");
                    }
                    cond->seen_else = 1;
                    if (!cond->was_true) {
                        cond->was_true = 1;
                        tok_free(t);
                        skip_line_raw(s);
                        s->in_directive = 0;
                        return;
                    }
                } else if (strcmp(d, "endif") == 0) {
                    pop_cond(s);
                    tok_free(t);
                    skip_line_raw(s);
                    s->in_directive = 0;
                    return;
                }
            } else if (strcmp(d, "endif") == 0) {
                depth--;
            }
        }
        if (t->kind != TK_EOL) {
            skip_line_raw(s);
        }
        tok_free(t);
        s->in_directive = 0;
    }
}

static void handle_directive(Source *s) {
    Tok *t;
    Loc loc;
    const char *d;

    s->in_directive = 1;
    t = lex_token(s);
    loc = t->loc;
    if (t->kind == TK_EOL) {
        tok_free(t);
        s->in_directive = 0;
        return;
    }
    if (t->kind == TK_NUMBER) {
        do_line(s, t);
        s->in_directive = 0;
        return;
    }
    if (t->kind != TK_IDENT) {
        diag_error(loc, "invalid preprocessing directive");
        tok_free(t);
        skip_to_eol(s);
        s->in_directive = 0;
        return;
    }
    d = t->text;
    if (strcmp(d, "define") == 0) {
        tok_free(t);
        do_define(s);
    } else if (strcmp(d, "undef") == 0) {
        tok_free(t);
        do_undef(s);
    } else if (strcmp(d, "include") == 0) {
        tok_free(t);
        do_include(s, loc, 0, 0);
    } else if (strcmp(d, "include_next") == 0) {
        tok_free(t);
        do_include(s, loc, 1, 0);
    } else if (strcmp(d, "import") == 0) {
        tok_free(t);
        do_include(s, loc, 0, 1);
    } else if (strcmp(d, "if") == 0) {
        int v;
        tok_free(t);
        v = eval_if(s, loc);
        push_cond(s, v, loc);
        s->in_directive = 0;
        if (!v) {
            skip_group(s);
        }
    } else if (strcmp(d, "ifdef") == 0 || strcmp(d, "ifndef") == 0) {
        int want = d[2] == 'd';
        int v;
        Tok *name;
        tok_free(t);
        name = lex_token(s);
        if (name->kind != TK_IDENT) {
            diag_error(name->loc, "macro name must be an identifier");
            v = 0;
            discard_line(s, name);
        } else {
            v = (macro_lookup(name->text) != NULL) == want;
            tok_free(name);
            expect_eol(s, want ? "ifdef" : "ifndef");
        }
        push_cond(s, v, loc);
        s->in_directive = 0;
        if (!v) {
            skip_group(s);
        }
    } else if (strcmp(d, "elif") == 0 || strcmp(d, "else") == 0) {
        /* reached while taking a group, so the rest of the conditional is skipped */
        int is_else = d[2] == 's';
        if (!s->conds) {
            diag_error(loc, "#%s without #if", d);
            tok_free(t);
            skip_to_eol(s);
        } else {
            tok_free(t);
            if (is_else) {
                if (s->conds->seen_else) {
                    diag_error(loc, "#else after #else");
                }
                s->conds->seen_else = 1;
            }
            s->conds->was_true = 1;
            skip_line_raw(s);
            s->in_directive = 0;
            skip_group(s);
        }
    } else if (strcmp(d, "endif") == 0) {
        tok_free(t);
        if (!s->conds) {
            diag_error(loc, "#endif without #if");
            skip_to_eol(s);
        } else {
            pop_cond(s);
            expect_eol(s, "endif");
        }
    } else if (strcmp(d, "line") == 0) {
        tok_free(t);
        do_line(s, lex_token(s));
    } else if (strcmp(d, "error") == 0) {
        char *text;
        tok_free(t);
        text = read_raw_line(s);
        diag_error(loc, "%s", text);
        free(text);
    } else if (strcmp(d, "warning") == 0) {
        char *text;
        tok_free(t);
        text = read_raw_line(s);
        diag_warning(loc, "%s", text);
        free(text);
    } else if (strcmp(d, "pragma") == 0) {
        tok_free(t);
        do_pragma(s, loc);
    } else if (strcmp(d, "ident") == 0 || strcmp(d, "sccs") == 0 ||
               strcmp(d, "assert") == 0 || strcmp(d, "unassert") == 0) {
        tok_free(t);
        skip_to_eol(s);
    } else {
        diag_error(loc, "invalid preprocessing directive '#%s'", d);
        tok_free(t);
        skip_to_eol(s);
    }
    s->in_directive = 0;
}

/* ------------------------------------------------------------------ */
/* setup and teardown                                                 */
/* ------------------------------------------------------------------ */

static void add_dir(const char *dir) {
    P.dirs = pp_realloc(P.dirs, (P.ndirs + 1) * sizeof(*P.dirs));
    P.dirs[P.ndirs++] = pp_strdup(dir);
}

static int std_is_strict(const char *std) {
    if (!std) {
        return 1;
    }
    return strncmp(std, "gnu", 3) != 0;
}

static const char *std_version(const char *std) {
    if (!std) {
        return NULL;
    }
    if (strstr(std, "99") || strstr(std, "9x")) {
        return "199901L";
    }
    if (strstr(std, "11") || strstr(std, "1x")) {
        return "201112L";
    }
    if (strstr(std, "17") || strstr(std, "18")) {
        return "201710L";
    }
    if (strstr(std, "94") || strstr(std, "199409")) {
        return "199409L";
    }
    return NULL;
}

/* the predefined macros, as a virtual file of #define lines */
static void push_builtin_source(const CppOptions *opts) {
    Buf b;
    const char *const *p;
    const char *ver = std_version(opts->std);
    size_t i;
    memset(&b, 0, sizeof(b));
    for (p = P.target->predefined; p && *p; p++) {
        buf_puts(&b, "#define ");
        buf_puts(&b, *p);
        buf_push(&b, '\n');
    }
    if (!P.strict) {
        for (p = P.target->nonstrict; p && *p; p++) {
            buf_puts(&b, "#define ");
            buf_puts(&b, *p);
            buf_push(&b, '\n');
        }
    } else {
        buf_puts(&b, "#define __STRICT_ANSI__ 1\n");
    }
    if (ver) {
        buf_puts(&b, "#define __STDC_VERSION__ ");
        buf_puts(&b, ver);
        buf_push(&b, '\n');
    }
    {
        char *buf = pp_strndup(b.s ? b.s : "", b.len);
        push_source(buf, b.len, "<built-in>", NULL, -1, 0);
        P.src->is_virtual = 1;
    }
    buf_reset(&b);
    /* -D and -U, in order */
    for (i = 0; i < opts->n_macros; i++) {
        const char *a = opts->macros[i];
        if (a[1] == 'D') {
            const char *eq = strchr(a + 2, '=');
            buf_puts(&b, "#define ");
            if (eq) {
                char *name = pp_strndup(a + 2, (size_t)(eq - (a + 2)));
                buf_puts(&b, name);
                buf_push(&b, ' ');
                buf_puts(&b, eq + 1);
                free(name);
            } else {
                buf_puts(&b, a + 2);
                buf_puts(&b, " 1");
            }
            buf_push(&b, '\n');
        } else if (a[1] == 'U') {
            buf_puts(&b, "#undef ");
            buf_puts(&b, a + 2);
            buf_push(&b, '\n');
        }
    }
    if (b.len) {
        /* pushed above the main file but below the built-ins, so it runs second */
        Source *builtin = P.src;
        char *buf = pp_strndup(b.s, b.len);
        P.src = builtin->prev;
        P.depth--;
        push_source(buf, b.len, "<command line>", NULL, -1, 0);
        P.src->is_virtual = 1;
        builtin->prev = P.src;
        P.src = builtin;
        P.depth++;
    }
    buf_free(&b);
}

static void setup(const CppOptions *opts) {
    const char *const *p;
    size_t i;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    const char *sysroot;
    size_t sn;

    memset(&P, 0, sizeof(P));
    P.target = target->cpp;
    P.strict = std_is_strict(opts->std);
    P.read = opts->read;
    P.read_user = opts->read_user;
    P.prev_kind = TK_EOF;
    if (tm) {
        strftime(P.date, sizeof(P.date), "\"%b %e %Y\"", tm);
        strftime(P.time, sizeof(P.time), "\"%H:%M:%S\"", tm);
    } else {
        strcpy(P.date, "\"??? ?? ????\"");
        strcpy(P.time, "\"??:??:??\"");
    }

    for (i = 0; i < opts->n_include_dirs; i++) {
        add_dir(opts->include_dirs[i]);
    }
    P.nuser = P.ndirs;
    sysroot = opts->sysroot ? opts->sysroot : P.target->default_sysroot;
    sn = strlen(sysroot);
    while (sn > 1 && sysroot[sn - 1] == '/') {
        sn--;
    }
    if (!opts->nostdinc) {
        for (p = P.target->system_dirs; p && *p; p++) {
            if ((*p)[0] == '=') {
                char *d = pp_alloc(sn + strlen(*p) + 1);
                memcpy(d, sysroot, sn);
                strcpy(d + sn, *p + 1);
                add_dir(d);
                free(d);
            } else {
                add_dir(*p);
            }
        }
    }

    macro_builtin("__FILE__", B_FILE, 0);
    macro_builtin("__LINE__", B_LINE, 0);
    macro_builtin("__DATE__", B_DATE, 0);
    macro_builtin("__TIME__", B_TIME, 0);
    macro_builtin("__COUNTER__", B_COUNTER, 0);
    macro_builtin("__INCLUDE_LEVEL__", B_INCLUDE_LEVEL, 0);
    macro_builtin("__BASE_FILE__", B_BASE_FILE, 0);
    macro_builtin("__TIMESTAMP__", B_TIMESTAMP, 0);
    macro_builtin("_Pragma", B_PRAGMA, 1);
    for (p = P.target->has_builtins; p && *p; p++) {
        Builtin b = B_HAS_QUERY;
        if (strcmp(*p, "__has_include") == 0) {
            b = B_HAS_INCLUDE;
        } else if (strcmp(*p, "__has_include_next") == 0) {
            b = B_HAS_INCLUDE_NEXT;
        }
        macro_builtin(*p, b, 1);
    }
}

static void teardown(void) {
    size_t i;
    Name *n;
    while (P.src) {
        pop_source();
    }
    for (i = 0; i < HASH_SIZE; i++) {
        Macro *m = P.table[i];
        while (m) {
            Macro *next = m->next;
            macro_free(m);
            m = next;
        }
    }
    for (i = 0; i < P.ndirs; i++) {
        free(P.dirs[i]);
    }
    free(P.dirs);
    while (P.pushed) {
        PushedMacro *pm = P.pushed;
        P.pushed = pm->next;
        if (pm->macro) {
            macro_free(pm->macro);
        }
        free(pm->name);
        free(pm);
    }
    for (i = 0; i < P.nonce; i++) {
        free(P.once[i]);
    }
    free(P.once);
    n = P.names;
    while (n) {
        Name *next = n->next;
        free(n->s);
        free(n);
        n = next;
    }
    buf_free(&P.buf);
    buf_free(&P.out);
    memset(&P, 0, sizeof(P));
}

char *cpp_text(const char *path, const CppOptions *opts, size_t *len) {
    int errors_before = diag_error_count();
    char *buf;
    char *text;
    size_t n;
    Expander x;
    Tok *t;

    setup(opts);
    buf = read_file(path, &n);
    if (!buf) {
        Loc loc;
        loc.file = intern(path);
        loc.line = 0;
        loc.col = 0;
        diag_error(loc, "cannot read '%s'", path);
        teardown();
        return NULL;
    }
    push_source(buf, n, path, path, -1, 0);
    P.base_file = P.src->name;
    push_builtin_source(opts);

    P.out_bol = 1;
    P.out_col = 0;
    P.need_marker = 1;
    x.ctx = NULL;
    x.from_file = 1;
    x.pending_ws = 0;
    while ((t = expand_next(&x)) != NULL) {
        if (t->kind == TK_EOL) {
            tok_free(t);
            continue;
        }
        emit(t);
        tok_free(t);
    }
    x_free(&x);
    if (!P.out_bol) {
        buf_push(&P.out, '\n');
    }
    if (diag_error_count() != errors_before) {
        teardown();
        return NULL;
    }
    text = P.out.s ? P.out.s : pp_alloc(1);
    *len = P.out.len;
    P.out.s = NULL;
    P.out.len = P.out.cap = 0;
    teardown();
    return text;
}
