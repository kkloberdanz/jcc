#ifndef TOKEN_H
#define TOKEN_H

#include <stddef.h>

/*
 * Every kind of token the lexer can produce. The list is an X-macro so the
 * enum and its string representation (kind_repr) stay in sync.
 */
#define TOKEN_KINDS(X) \
    X(TOK_EOF) \
    X(TOK_ID) \
    X(TOK_INT_LIT) \
    X(TOK_FLOAT_LIT) \
    X(TOK_CHAR_LIT) \
    X(TOK_STRING) \
    /* ANSI C keywords */ \
    X(TOK_AUTO) \
    X(TOK_BREAK) \
    X(TOK_CASE) \
    X(TOK_CHAR) \
    X(TOK_CONST) \
    X(TOK_CONTINUE) \
    X(TOK_DEFAULT) \
    X(TOK_DO) \
    X(TOK_DOUBLE) \
    X(TOK_ELSE) \
    X(TOK_ENUM) \
    X(TOK_EXTERN) \
    X(TOK_FLOAT) \
    X(TOK_FOR) \
    X(TOK_GOTO) \
    X(TOK_IF) \
    X(TOK_INT) \
    X(TOK_LONG) \
    X(TOK_REGISTER) \
    X(TOK_RETURN) \
    X(TOK_SHORT) \
    X(TOK_SIGNED) \
    X(TOK_SIZEOF) \
    X(TOK_STATIC) \
    X(TOK_STRUCT) \
    X(TOK_SWITCH) \
    X(TOK_TYPEDEF) \
    X(TOK_UNION) \
    X(TOK_UNSIGNED) \
    X(TOK_VOID) \
    X(TOK_VOLATILE) \
    X(TOK_WHILE) \
    /* compiler extensions found in system headers */ \
    X(TOK_BUILTIN_VA_LIST) \
    X(TOK_BUILTIN_VA_ARG) \
    X(TOK_INLINE) \
    X(TOK_RESTRICT) \
    X(TOK_ATTRIBUTE) \
    X(TOK_ASM) \
    X(TOK_NULLABILITY) \
    X(TOK_INT128) \
    X(TOK_BOOL) \
    X(TOK_FLOAT16) \
    X(TOK_BUILTIN_OFFSETOF) \
    X(TOK_UINT128) \
    X(TOK_EXTENSION) \
    X(TOK_PRAGMA) \
    X(TOK_COMPLEX) \
    X(TOK_REAL) \
    X(TOK_IMAG) \
    /* punctuators */ \
    X(TOK_ELLIPSIS) \
    X(TOK_RIGHT_ASSIGN) \
    X(TOK_LEFT_ASSIGN) \
    X(TOK_PLUS_ASSIGN) \
    X(TOK_MINUS_ASSIGN) \
    X(TOK_MUL_ASSIGN) \
    X(TOK_DIV_ASSIGN) \
    X(TOK_MOD_ASSIGN) \
    X(TOK_AND_ASSIGN) \
    X(TOK_XOR_ASSIGN) \
    X(TOK_OR_ASSIGN) \
    X(TOK_RIGHT_SHIFT) \
    X(TOK_LEFT_SHIFT) \
    X(TOK_INC) \
    X(TOK_DEC) \
    X(TOK_ARROW) \
    X(TOK_AND_OP) \
    X(TOK_OR_OP) \
    X(TOK_LE) \
    X(TOK_GE) \
    X(TOK_EQ) \
    X(TOK_NE) \
    X(TOK_SEMICOLON) \
    X(TOK_LBRACE) \
    X(TOK_RBRACE) \
    X(TOK_COMMA) \
    X(TOK_COLON) \
    X(TOK_ASSIGN) \
    X(TOK_LPAREN) \
    X(TOK_RPAREN) \
    X(TOK_LBRACK) \
    X(TOK_RBRACK) \
    X(TOK_DOT) \
    X(TOK_AMPERSAND) \
    X(TOK_NOT) \
    X(TOK_TILDE) \
    X(TOK_MINUS) \
    X(TOK_PLUS) \
    X(TOK_STAR) \
    X(TOK_SLASH) \
    X(TOK_PERCENT) \
    X(TOK_LT) \
    X(TOK_GT) \
    X(TOK_HAT) \
    X(TOK_PIPE) \
    X(TOK_QUESTION)

#define TOKEN_KIND_ENUM(K) K,
typedef enum TokenKind {
    TOKEN_KINDS(TOKEN_KIND_ENUM)
    TOK_NKINDS
} TokenKind;
#undef TOKEN_KIND_ENUM

typedef struct Token {
    size_t lineno;
    size_t col;
    char *repr;
    TokenKind kind;
    const char *file; /* source file name, owned by the lexer */
    const char *line; /* full text of the source line, owned by the lexer */
} Token;

typedef struct TokList {
    Token tok;
    struct TokList *next;
} TokList;

TokList *toklist_new(Token tok);

TokList *toklist_insert(TokList *before, TokList *new);

TokList *toklist_push(TokList *before, TokList *new);

void toklist_print(TokList *head);

void toklist_free(TokList *head);

void tok_print(struct Token *tok);

void tok_init(
    Token *tok,
    size_t lineno,
    size_t col,
    const char *repr,
    TokenKind kind
);

void token_free(Token *tok);

/* Look up a keyword; returns TOK_ID when the word is not a keyword */
TokenKind keyword_kind(const char *word);

/*
 * Match the longest punctuator at the start of s. On success, returns its
 * kind and stores its length in *len. Returns TOK_EOF when nothing matches.
 */
TokenKind punctuator_kind(const char *s, size_t *len);

const char *kind_repr(TokenKind kind);

#endif /* TOKEN_H */
