/*
 * Lexer for preprocessed C.
 *
 * The input is the output of the C preprocessor: comments are gone, line
 * splices are resolved, and `# lineno "file"` markers tell us where each
 * line came from. The whole input is read into memory first so every token
 * can point back at the line and file it came from for error messages.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lex.h"
#include "diag.h"
#include "util.h"

typedef struct Line {
    char *text;
    size_t lineno;
    const char *file;
    int pragma;         /* a `#pragma pack` directive: text is its argument, one token */
} Line;

static const char *input = NULL;

static Line *lines = NULL;
static size_t nlines = 0;
static size_t lines_cap = 0;

/* interned file names so tokens can share them */
static char **files = NULL;
static size_t nfiles = 0;
static size_t files_cap = 0;

/* cursor into lines[] while tokenizing */
static size_t cur_line = 0;
static size_t cur_col = 0;

/* growable buffer used to build the text of the token in progress */
static char *buf = NULL;
static size_t buf_len = 0;
static size_t buf_cap = 0;

static void text_reset(void) {
    buf_len = 0;
    if (buf) {
        buf[0] = '\0';
    }
}

static void text_push(char c) {
    if (buf_len + 2 > buf_cap) {
        buf_cap = buf_cap ? buf_cap * 2 : 128;
        buf = xrealloc(buf, buf_cap);
    }
    buf[buf_len++] = c;
    buf[buf_len] = '\0';
}

static const char *intern_file(const char *name) {
    size_t i;
    for (i = 0; i < nfiles; i++) {
        if (strcmp(files[i], name) == 0) {
            return files[i];
        }
    }
    if (nfiles == files_cap) {
        files_cap = files_cap ? files_cap * 2 : 16;
        files = xrealloc(files, files_cap * sizeof(*files));
    }
    files[nfiles] = strdup(name);
    return files[nfiles++];
}

static void push_line(char *text, size_t lineno, const char *file) {
    if (nlines == lines_cap) {
        lines_cap = lines_cap ? lines_cap * 2 : 256;
        lines = xrealloc(lines, lines_cap * sizeof(*lines));
    }
    lines[nlines].text = text;
    lines[nlines].lineno = lineno;
    lines[nlines].file = file;
    lines[nlines].pragma = 0;
    nlines++;
}

/*
 * Handle a preprocessor directive that survived preprocessing. Line markers
 * look like `# 12 "file.c" 1 3 4`. `#pragma pack(...)` becomes a line of
 * its own that lexes as one TOK_PRAGMA token, so the parser sees it in
 * order with the declarations. Every other directive (#pragma once,
 * #ident, ...) is ignored.
 */
static void lex_directive(const char *text, size_t *lineno, const char **file) {
    const char *p = text;
    char *end = NULL;
    unsigned long n;

    p++; /* skip '#' */
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (strncmp(p, "pragma", 6) == 0 && (p[6] == ' ' || p[6] == '\t')) {
        p += 6;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (strncmp(p, "pack", 4) == 0 && !isalnum((unsigned char)p[4]) && p[4] != '_') {
            size_t len = strlen(p);
            char *copy;
            while (len && (p[len - 1] == '\n' || p[len - 1] == ' ' || p[len - 1] == '\t')) {
                len--;
            }
            copy = xrealloc(NULL, len + 1);
            memcpy(copy, p, len);
            copy[len] = '\0';
            push_line(copy, *lineno, *file);
            lines[nlines - 1].pragma = 1;
        }
        return;
    }
    if (!isdigit((unsigned char)*p)) {
        return;
    }
    n = strtoul(p, &end, 10);
    p = end;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '"') {
        const char *start = ++p;
        text_reset();
        while (*p && *p != '"') {
            text_push(*p++);
        }
        if (p > start || *p == '"') {
            *file = intern_file(buf);
        }
    }
    *lineno = (size_t)n;
}

static void read_input(void) {
    const char *p = input;
    size_t lineno = 1;
    const char *file = intern_file("<stdin>");

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) + 1 : strlen(p);
        char *text = xrealloc(NULL, n + 1);
        const char *q = text;
        memcpy(text, p, n);
        text[n] = '\0';
        p += n;
        while (*q == ' ' || *q == '\t') {
            q++;
        }
        if (*q == '#') {
            lex_directive(q, &lineno, &file);
            free(text);
            continue;
        }
        push_line(text, lineno, file);
        lineno++;
    }
}

static void lex_error(const char *msg);

/* character access through the cursor */

static int at_end(void) {
    return cur_line >= nlines;
}

static int peekc(void) {
    if (at_end()) {
        return '\0';
    }
    return lines[cur_line].text[cur_col];
}

static int peekc_at(size_t ahead) {
    const char *text;
    size_t i;
    if (at_end()) {
        return '\0';
    }
    text = lines[cur_line].text;
    for (i = 0; i < ahead; i++) {
        if (text[cur_col + i] == '\0') {
            return '\0';
        }
    }
    return text[cur_col + ahead];
}

static void next_char(void) {
    if (at_end()) {
        return;
    }
    if (lines[cur_line].text[cur_col] == '\0') {
        cur_line++;
        cur_col = 0;
    } else {
        cur_col++;
    }
}

/*
 * Skip whitespace and comments, moving across line boundaries. The
 * preprocessor strips comments, but clang leaves `//` comments from system
 * headers alone when run in C90 mode.
 */
static void skip_space(void) {
    while (!at_end()) {
        int c = peekc();
        if (c == '\0' || isspace(c)) {
            next_char();
        } else if (c == '/' && peekc_at(1) == '/') {
            while (!at_end() && peekc() != '\0') {
                next_char();
            }
        } else if (c == '/' && peekc_at(1) == '*') {
            next_char();
            next_char();
            while (!at_end() && !(peekc() == '*' && peekc_at(1) == '/')) {
                next_char();
            }
            if (at_end()) {
                lex_error("unterminated comment");
            }
            next_char();
            next_char();
        } else {
            break;
        }
    }
}

static void lex_error(const char *msg) {
    Loc loc;
    if (at_end()) {
        loc.file = nlines ? lines[nlines - 1].file : NULL;
        loc.line = nlines ? lines[nlines - 1].lineno : 0;
        loc.col = 0;
        diag_fatal(loc, "lexical error at end of input: %s", msg);
    }
    loc.file = lines[cur_line].file;
    loc.line = lines[cur_line].lineno;
    loc.col = cur_col;
    diag_fatal(loc, "lexical error: %s", msg);
}

/* copy a quoted literal (terminated by `quote`) into buf, escapes included */
static void scan_quoted(int quote) {
    text_push((char)peekc());
    next_char();
    for (;;) {
        int c = peekc();
        if (c == '\0') {
            lex_error("unterminated literal");
        }
        text_push((char)c);
        next_char();
        if (c == '\\') {
            c = peekc();
            if (c == '\0') {
                lex_error("unterminated literal");
            }
            text_push((char)c);
            next_char();
        } else if (c == quote) {
            break;
        }
    }
}

static TokenKind scan_number(void) {
    TokenKind kind = TOK_INT_LIT;
    int c = peekc();

    if (c == '0' && (peekc_at(1) == 'x' || peekc_at(1) == 'X')) {
        text_push((char)c);
        next_char();
        text_push((char)peekc());
        next_char();
        while (isxdigit(peekc())) {
            text_push((char)peekc());
            next_char();
        }
        /* a hexadecimal floating constant: 0x1.8p-3, binary exponent required */
        if (peekc() == '.') {
            kind = TOK_FLOAT_LIT;
            text_push('.');
            next_char();
            while (isxdigit(peekc())) {
                text_push((char)peekc());
                next_char();
            }
        }
        if (peekc() == 'p' || peekc() == 'P') {
            int sign = peekc_at(1);
            kind = TOK_FLOAT_LIT;
            text_push((char)peekc());
            next_char();
            if (sign == '+' || sign == '-') {
                text_push((char)sign);
                next_char();
            }
            if (!isdigit(peekc())) {
                lex_error("exponent has no digits");
            }
            while (isdigit(peekc())) {
                text_push((char)peekc());
                next_char();
            }
        } else if (kind == TOK_FLOAT_LIT) {
            lex_error("hexadecimal floating constant requires an exponent");
        }
    } else {
        while (isdigit(peekc())) {
            text_push((char)peekc());
            next_char();
        }
        if (peekc() == '.') {
            kind = TOK_FLOAT_LIT;
            text_push('.');
            next_char();
            while (isdigit(peekc())) {
                text_push((char)peekc());
                next_char();
            }
        }
        if (peekc() == 'e' || peekc() == 'E') {
            int sign = peekc_at(1);
            int digit = (sign == '+' || sign == '-') ? peekc_at(2) : sign;
            if (isdigit(digit)) {
                kind = TOK_FLOAT_LIT;
                text_push((char)peekc());
                next_char();
                if (sign == '+' || sign == '-') {
                    text_push((char)sign);
                    next_char();
                }
                while (isdigit(peekc())) {
                    text_push((char)peekc());
                    next_char();
                }
            }
        }
    }

    /* integer and floating suffixes: u, l, f in any case; i or j (GNU) makes an imaginary constant */
    for (;;) {
        c = peekc();
        if (c == 'u' || c == 'U' || c == 'l' || c == 'L') {
            text_push((char)c);
            next_char();
        } else if ((c == 'f' || c == 'F') && kind == TOK_FLOAT_LIT) {
            text_push((char)c);
            next_char();
        } else if (c == 'i' || c == 'I' || c == 'j' || c == 'J') {
            text_push((char)c);
            next_char();
            kind = TOK_FLOAT_LIT;   /* its type is complex, of a floating type */
        } else {
            break;
        }
    }

    if (isalnum(peekc()) || peekc() == '_') {
        lex_error("invalid suffix on numeric constant");
    }
    return kind;
}

/*
 * Append a parenthesised group to buf, including the outer parentheses,
 * honouring nested parentheses and string or character literals. Used to
 * swallow the arguments of __attribute__ and __asm as a single token.
 */
static void scan_paren_group(void) {
    int depth = 0;

    skip_space();
    if (peekc() != '(') {
        lex_error("expected '('");
    }
    for (;;) {
        int c = peekc();
        if (at_end()) {
            lex_error("unterminated parenthesised group");
        }
        if (c == '\0' || c == '\n') {
            /* fold line breaks into a single space */
            if (buf_len && buf[buf_len - 1] != ' ') {
                text_push(' ');
            }
            next_char();
            continue;
        }
        if (c == '"' || c == '\'') {
            scan_quoted(c);
            continue;
        }
        text_push((char)c);
        next_char();
        if (c == '(') {
            depth++;
        } else if (c == ')') {
            depth--;
            if (depth == 0) {
                break;
            }
        }
    }
}

/*
 * Scan the next token into *out. Returns 0 at end of input.
 */
static int next_token(Token *out) {
    TokenKind kind;
    const Line *line;
    size_t col;
    int c;

    for (;;) {
        skip_space();
        if (at_end()) {
            return 0;
        }

        line = &lines[cur_line];
        col = cur_col;
        c = peekc();
        text_reset();

        if (line->pragma) {
            /* the whole line is the token's text */
            const char *s;
            for (s = line->text; *s; s++) {
                text_push(*s);
            }
            cur_line++;
            cur_col = 0;
            kind = TOK_PRAGMA;
        } else if (isalpha(c) || c == '_') {
            while (isalnum(peekc()) || peekc() == '_') {
                text_push((char)peekc());
                next_char();
            }
            if (strcmp(buf, "L") == 0 && (peekc() == '"' || peekc() == '\'')) {
                /* wide literal */
                int quote = peekc();
                scan_quoted(quote);
                kind = quote == '"' ? TOK_STRING : TOK_CHAR_LIT;
            } else {
                kind = keyword_kind(buf);
                if (kind == TOK_ATTRIBUTE || kind == TOK_ASM) {
                    scan_paren_group();
                } else if (kind == TOK_EXTENSION) {
                    /* __extension__ only silences warnings; drop it */
                    continue;
                }
            }
        } else if (isdigit(c) || (c == '.' && isdigit(peekc_at(1)))) {
            kind = scan_number();
        } else if (c == '"') {
            scan_quoted('"');
            kind = TOK_STRING;
        } else if (c == '\'') {
            scan_quoted('\'');
            kind = TOK_CHAR_LIT;
        } else {
            size_t len;
            size_t i;
            kind = punctuator_kind(&line->text[col], &len);
            if (len == 0) {
                lex_error("unexpected character");
            }
            for (i = 0; i < len; i++) {
                text_push((char)peekc());
                next_char();
            }
        }
        break;
    }

    tok_init(out, line->lineno, col, buf, kind);
    out->file = line->file;
    out->line = line->text;
    return 1;
}

void lex_set_text(const char *text) {
    size_t i;
    /* a new translation unit: forget the previous one's lines (file names stay interned: tokens may still point at them) */
    for (i = 0; i < nlines; i++) {
        free(lines[i].text);
    }
    nlines = 0;
    cur_line = 0;
    cur_col = 0;
    input = text;
}

/* the list lex() is building: a fatal error leaves it here for lex_cleanup() */
static TokList *lexing = NULL;

void lex_cleanup(void) {
    toklist_free(lexing);
    lexing = NULL;
}

TokList *lex(void) {
    Token tok;
    TokList *head = NULL;
    TokList *tail = NULL;

    if (!input) {
        fatal("internal error: lex called before lex_set_text");
    }

    read_input();
    cur_line = 0;
    cur_col = 0;

    while (next_token(&tok)) {
        TokList *node = toklist_new(tok);
        if (!node) {
            fatal("out of memory");
        }
        if (tail) {
            tail->next = node;
        } else {
            head = node;
            lexing = head;
        }
        tail = node;
    }
    lexing = NULL;

    tok_init(&tok, nlines ? lines[nlines - 1].lineno : 0, 0, "EOF", TOK_EOF);
    if (nlines) {
        tok.file = lines[nlines - 1].file;
        tok.line = lines[nlines - 1].text;
    }
    if (tail) {
        tail->next = toklist_new(tok);
    } else {
        head = toklist_new(tok);
    }
    return head;
}
