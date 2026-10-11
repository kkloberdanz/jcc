#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "token.h"
#include "util.h"

TokList *toklist_new(Token tok) {
    TokList *ll = malloc(sizeof(*ll));
    if (ll) {
        ll->next = NULL;
        ll->tok = tok;
    }
    return ll;
}

TokList *toklist_insert(TokList *before, TokList *new) {
    if (before) {
        TokList *next = before->next;
        before->next = new;
        new->next = next;
    }
    return new;
}

TokList *toklist_push(TokList *before, TokList *new) {
    if (before) {
        while (before->next) {
            before = before->next;
        }
        before->next = new;
    }
    return new;
}

void toklist_free(TokList *head) {
    while (head) {
        TokList *next = head->next;
        token_free(&head->tok);
        free(head);
        head = next;
    }
}

#define TOKEN_KIND_NAME(K) #K,
static const char * const kind_names[] = {
    TOKEN_KINDS(TOKEN_KIND_NAME)
};
#undef TOKEN_KIND_NAME

const char *kind_repr(TokenKind kind) {
    if ((size_t)kind < sizeof(kind_names) / sizeof(*kind_names)) {
        return kind_names[kind];
    }
    return "(invalid tok)";
}

void tok_print(struct Token *tok) {
    printf(
        "{tok: '%s', kind: %s, line: %zu, col: %zu}",
        tok->repr,
        kind_repr(tok->kind),
        tok->lineno,
        tok->col
    );
}

void toklist_print(TokList *head) {
    printf("[\n");
    if (head) {
        while (head->next) {
            printf("\t");
            tok_print(&head->tok);
            printf(",\n");
            head = head->next;
        }
        printf("\t");
        tok_print(&head->tok);
    }
    puts("\n]");
}

void tok_init(
    Token *out,
    size_t lineno,
    size_t col,
    const char *repr,
    TokenKind kind
) {
    out->lineno = lineno;
    out->col = col;
    out->repr = strdup(repr);
    out->kind = kind;
    out->file = "";
    out->line = "";
}

void token_free(Token *tok) {
    if (tok) {
        free(tok->repr);
    }
}

struct Keyword {
    const char *word;
    TokenKind kind;
};

/*
 * ANSI C keywords plus the GNU/clang spellings that appear in system
 * headers. Alternative spellings map onto the same kind.
 */
static const struct Keyword keywords[] = {
    {"auto", TOK_AUTO},
    {"break", TOK_BREAK},
    {"case", TOK_CASE},
    {"char", TOK_CHAR},
    {"const", TOK_CONST},
    {"__const", TOK_CONST},
    {"__const__", TOK_CONST},
    {"continue", TOK_CONTINUE},
    {"default", TOK_DEFAULT},
    {"do", TOK_DO},
    {"double", TOK_DOUBLE},
    {"else", TOK_ELSE},
    {"enum", TOK_ENUM},
    {"extern", TOK_EXTERN},
    {"float", TOK_FLOAT},
    {"for", TOK_FOR},
    {"goto", TOK_GOTO},
    {"if", TOK_IF},
    {"int", TOK_INT},
    {"long", TOK_LONG},
    {"register", TOK_REGISTER},
    {"return", TOK_RETURN},
    {"short", TOK_SHORT},
    {"signed", TOK_SIGNED},
    {"__signed", TOK_SIGNED},
    {"__signed__", TOK_SIGNED},
    {"sizeof", TOK_SIZEOF},
    {"static", TOK_STATIC},
    {"struct", TOK_STRUCT},
    {"switch", TOK_SWITCH},
    {"typedef", TOK_TYPEDEF},
    {"union", TOK_UNION},
    {"unsigned", TOK_UNSIGNED},
    {"void", TOK_VOID},
    {"volatile", TOK_VOLATILE},
    {"__volatile", TOK_VOLATILE},
    {"__volatile__", TOK_VOLATILE},
    {"while", TOK_WHILE},

    {"__builtin_va_list", TOK_BUILTIN_VA_LIST},
    {"__builtin_va_arg", TOK_BUILTIN_VA_ARG},
    {"inline", TOK_INLINE},
    {"__inline", TOK_INLINE},
    {"__inline__", TOK_INLINE},
    {"restrict", TOK_RESTRICT},
    {"__restrict", TOK_RESTRICT},
    {"__restrict__", TOK_RESTRICT},
    {"__attribute__", TOK_ATTRIBUTE},
    {"__attribute", TOK_ATTRIBUTE},
    {"asm", TOK_ASM},
    {"__asm", TOK_ASM},
    {"__asm__", TOK_ASM},
    {"_Nullable", TOK_NULLABILITY},
    {"_Nonnull", TOK_NULLABILITY},
    {"_Null_unspecified", TOK_NULLABILITY},
    {"__nullable", TOK_NULLABILITY},
    {"__nonnull", TOK_NULLABILITY},
    {"__null_unspecified", TOK_NULLABILITY},
    {"_Bool", TOK_BOOL},
    {"_Float16", TOK_FLOAT16},
    {"_Complex", TOK_COMPLEX},
    {"__complex__", TOK_COMPLEX},
    {"__complex", TOK_COMPLEX},
    {"_Imaginary", TOK_COMPLEX},
    {"__real__", TOK_REAL},
    {"__real", TOK_REAL},
    {"__imag__", TOK_IMAG},
    {"__imag", TOK_IMAG},
    {"__builtin_offsetof", TOK_BUILTIN_OFFSETOF},
    {"__int128", TOK_INT128},
    {"__int128_t", TOK_INT128},
    {"__uint128_t", TOK_UINT128},
    {"__extension__", TOK_EXTENSION}
};

TokenKind keyword_kind(const char *word) {
    size_t i;
    for (i = 0; i < sizeof(keywords) / sizeof(*keywords); i++) {
        if (strcmp(word, keywords[i].word) == 0) {
            return keywords[i].kind;
        }
    }
    return TOK_ID;
}

/* ordered longest first so the first match is the longest match */
static const struct Keyword punctuators[] = {
    {"...", TOK_ELLIPSIS},
    {">>=", TOK_RIGHT_ASSIGN},
    {"<<=", TOK_LEFT_ASSIGN},
    {"+=", TOK_PLUS_ASSIGN},
    {"-=", TOK_MINUS_ASSIGN},
    {"*=", TOK_MUL_ASSIGN},
    {"/=", TOK_DIV_ASSIGN},
    {"%=", TOK_MOD_ASSIGN},
    {"&=", TOK_AND_ASSIGN},
    {"^=", TOK_XOR_ASSIGN},
    {"|=", TOK_OR_ASSIGN},
    {">>", TOK_RIGHT_SHIFT},
    {"<<", TOK_LEFT_SHIFT},
    {"++", TOK_INC},
    {"--", TOK_DEC},
    {"->", TOK_ARROW},
    {"&&", TOK_AND_OP},
    {"||", TOK_OR_OP},
    {"<=", TOK_LE},
    {">=", TOK_GE},
    {"==", TOK_EQ},
    {"!=", TOK_NE},
    {";", TOK_SEMICOLON},
    {"{", TOK_LBRACE},
    {"}", TOK_RBRACE},
    {",", TOK_COMMA},
    {":", TOK_COLON},
    {"=", TOK_ASSIGN},
    {"(", TOK_LPAREN},
    {")", TOK_RPAREN},
    {"[", TOK_LBRACK},
    {"]", TOK_RBRACK},
    {".", TOK_DOT},
    {"&", TOK_AMPERSAND},
    {"!", TOK_NOT},
    {"~", TOK_TILDE},
    {"-", TOK_MINUS},
    {"+", TOK_PLUS},
    {"*", TOK_STAR},
    {"/", TOK_SLASH},
    {"%", TOK_PERCENT},
    {"<", TOK_LT},
    {">", TOK_GT},
    {"^", TOK_HAT},
    {"|", TOK_PIPE},
    {"?", TOK_QUESTION}
};

TokenKind punctuator_kind(const char *s, size_t *len) {
    size_t i;
    for (i = 0; i < sizeof(punctuators) / sizeof(*punctuators); i++) {
        size_t n = strlen(punctuators[i].word);
        if (strncmp(s, punctuators[i].word, n) == 0) {
            *len = n;
            return punctuators[i].kind;
        }
    }
    *len = 0;
    return TOK_EOF;
}
