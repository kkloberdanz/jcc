#ifndef AST_H
#define AST_H

#include <stddef.h>
#include <stdio.h>

#include "type.h"

/* Where a node came from. `file` is interned by the lexer. */
typedef struct Loc {
    const char *file;
    size_t line;
    size_t col;
} Loc;

/*
 * Operators. The X-macro keeps the enum and its printable spelling in sync.
 */
#define OP_KINDS(X) \
    X(OP_NOP, "nop") \
    /* binary */ \
    X(OP_ADD, "+") \
    X(OP_SUB, "-") \
    X(OP_MUL, "*") \
    X(OP_DIV, "/") \
    X(OP_MOD, "%") \
    X(OP_SHL, "<<") \
    X(OP_SHR, ">>") \
    X(OP_LT, "<") \
    X(OP_GT, ">") \
    X(OP_LE, "<=") \
    X(OP_GE, ">=") \
    X(OP_EQ, "==") \
    X(OP_NE, "!=") \
    X(OP_BITAND, "&") \
    X(OP_BITXOR, "^") \
    X(OP_BITOR, "|") \
    X(OP_AND, "&&") \
    X(OP_OR, "||") \
    X(OP_COMMA, ",") \
    /* assignment */ \
    X(OP_ASSIGN, "=") \
    X(OP_MUL_ASSIGN, "*=") \
    X(OP_DIV_ASSIGN, "/=") \
    X(OP_MOD_ASSIGN, "%=") \
    X(OP_ADD_ASSIGN, "+=") \
    X(OP_SUB_ASSIGN, "-=") \
    X(OP_SHL_ASSIGN, "<<=") \
    X(OP_SHR_ASSIGN, ">>=") \
    X(OP_AND_ASSIGN, "&=") \
    X(OP_XOR_ASSIGN, "^=") \
    X(OP_OR_ASSIGN, "|=") \
    /* unary */ \
    X(OP_ADDR, "&") \
    X(OP_DEREF, "*") \
    X(OP_POS, "+") \
    X(OP_NEG, "-") \
    X(OP_BITNOT, "~") \
    X(OP_NOT, "!") \
    X(OP_PREINC, "++") \
    X(OP_PREDEC, "--") \
    X(OP_POSTINC, "post++") \
    X(OP_POSTDEC, "post--") \
    X(OP_REAL, "__real__") \
    X(OP_IMAG, "__imag__")

typedef enum OpKind {
#define X(name, repr) name,
    OP_KINDS(X)
#undef X
    OP_NKINDS
} OpKind;

typedef enum StorageKind {
    STORAGE_TYPEDEF,
    STORAGE_EXTERN,
    STORAGE_STATIC,
    STORAGE_AUTO,
    STORAGE_REGISTER,
    STORAGE_INLINE,
    STORAGE_NONE    /* no storage class specifier; only used on symbols */
} StorageKind;

typedef enum QualKind {
    QUAL_CONST,
    QUAL_VOLATILE,
    QUAL_RESTRICT,
    QUAL_NULLABILITY
} QualKind;

/*
 * Node kinds. The comment after each one says which member of the union
 * it uses and what the fields mean.
 */
#define NODE_KINDS(X) \
    /* expressions */ \
    X(N_IDENT, "Ident")                  /* name */ \
    X(N_INT, "Int")                      /* text: literal as written */ \
    X(N_FLOAT, "Float")                  /* text */ \
    X(N_CHAR, "Char")                    /* text, quotes included */ \
    X(N_STRING, "String")                /* text, quotes included */ \
    X(N_UNARY, "Unary")                  /* unary: op, operand */ \
    X(N_BINARY, "Binary")                /* binary: op, left, right */ \
    X(N_ASSIGN, "Assign")                /* binary: op, left, right */ \
    X(N_COND, "Cond")                    /* cond: cond ? then : otherwise */ \
    X(N_CALL, "Call")                    /* call: func, args */ \
    X(N_INDEX, "Index")                  /* index: array, index */ \
    X(N_MEMBER, "Member")                /* member: object, name, arrow */ \
    X(N_CAST, "Cast")                    /* cast: type, expr */ \
    X(N_SIZEOF_EXPR, "SizeofExpr")       /* cast: expr */ \
    X(N_SIZEOF_TYPE, "SizeofType")       /* cast: type */ \
    X(N_IMPLICIT_CAST, "ImplicitCast")   /* cast: expr; inserted by sema, `type` is the target */ \
    X(N_VA_ARG, "VaArg")                /* cast: expr = the va_list, type = the TypeName */ \
    X(N_STMT_EXPR, "StmtExpr")           /* stmt: body = a Compound; GNU `({ ... })` */ \
    X(N_OFFSETOF, "Offsetof")            /* desig: init = the TypeName, path = the member designator */ \
    /* statements */ \
    X(N_LABEL, "Label")                  /* stmt: name, body */ \
    X(N_CASE, "Case")                    /* stmt: expr, body */ \
    X(N_DEFAULT, "Default")              /* stmt: body */ \
    X(N_COMPOUND, "Compound")            /* block: items (declarations then statements), scope */ \
    X(N_EXPR_STMT, "ExprStmt")           /* stmt: expr (NULL for `;`) */ \
    X(N_IF, "If")                        /* cond: cond, then, otherwise (may be NULL) */ \
    X(N_SWITCH, "Switch")                /* stmt: expr, body */ \
    X(N_WHILE, "While")                  /* stmt: expr, body */ \
    X(N_DO, "Do")                        /* stmt: body, expr */ \
    X(N_FOR, "For")                      /* loop: init, cond, step, body; any but body may be NULL */ \
    X(N_GOTO, "Goto")                    /* stmt: name */ \
    X(N_CONTINUE, "Continue")            /* - */ \
    X(N_BREAK, "Break")                  /* - */ \
    X(N_RETURN, "Return")                /* stmt: expr (may be NULL) */ \
    /* declarations */ \
    X(N_TRANSLATION_UNIT, "TranslationUnit") /* list: external declarations */ \
    X(N_FUNCTION_DEF, "FunctionDef")     /* func: specs, declarator, kr_decls, body, scope */ \
    X(N_DECLARATION, "Declaration")      /* decl: specs, declarators (init declarators) */ \
    X(N_INIT_DECLARATOR, "InitDeclarator") /* init_decl: declarator, init (may be NULL) */ \
    X(N_INIT_LIST, "InitList")           /* list: initializers */ \
    X(N_DESIGNATED, "Designated")        /* desig: path (designators), init */ \
    X(N_DESIG_MEMBER, "DesigMember")     /* name: `.name` */ \
    X(N_DESIG_INDEX, "DesigIndex")       /* cast: expr = `[expr]` */ \
    X(N_DECL_NAME, "DeclName")           /* declarator: name, attrs */ \
    X(N_DECL_POINTER, "DeclPointer")     /* declarator: inner, quals, block, attrs */ \
    X(N_DECL_ARRAY, "DeclArray")         /* declarator: inner, size (may be NULL), attrs */ \
    X(N_DECL_FUNC, "DeclFunc")           /* declarator: inner, params, variadic, kr, attrs, scope */ \
    X(N_STORAGE_CLASS, "StorageClass")   /* storage */ \
    X(N_TYPE_SPEC, "TypeSpec")           /* type_spec */ \
    X(N_TYPE_QUAL, "TypeQual")           /* qual */ \
    X(N_TYPEDEF_NAME, "TypedefName")     /* name */ \
    X(N_ATTRIBUTE, "Attribute")          /* text: the whole __attribute__ or __asm */ \
    X(N_STRUCT_SPEC, "StructSpec")       /* record: is_union, tag, has_body, members, scope */ \
    X(N_STRUCT_DECLARATION, "StructDeclaration") /* decl: specs, declarators (struct declarators) */ \
    X(N_STRUCT_DECLARATOR, "StructDeclarator") /* init_decl: declarator (may be NULL), init = bit width */ \
    X(N_ENUM_SPEC, "EnumSpec")           /* record: tag, has_body, members (enumerators), underlying */ \
    X(N_ENUMERATOR, "Enumerator")        /* enumerator: name, value, attrs */ \
    X(N_PARAM, "Param")                  /* typed: specs, declarator (may be NULL or abstract) */ \
    X(N_TYPE_NAME, "TypeName")           /* typed: specs, declarator (abstract, may be NULL) */

typedef enum NodeKind {
#define X(name, repr) name,
    NODE_KINDS(X)
#undef X
    N_NKINDS
} NodeKind;

typedef struct AST AST;
struct Scope;   /* src/symtab.h */
struct Symbol;  /* src/symtab.h */
struct Member;  /* src/type.h */

typedef struct ASTList {
    AST **items;
    size_t len;
    size_t cap;
} ASTList;

/*
 * Declarators are a chain from the base type inward to the name. For
 * `int *a[3]` the chain is DeclPointer -> DeclArray(3) -> DeclName(a):
 * start from `int`, make a pointer to it, an array of 3 of that, and call
 * it `a`. Abstract declarators end in a NULL inner instead of a DeclName.
 */
struct AST {
    NodeKind kind;
    Loc loc;
    /*
     * Filled in by semantic analysis (src/sema.h). `type` is the type of
     * an expression, or of a TypeName or Param. `sym` is the symbol a
     * node declares (InitDeclarator, Param, Enumerator, Label, FunctionDef,
     * StructSpec, EnumSpec, TypedefName) or refers to (Ident, Goto); the
     * parser sets it where it can. `lvalue` marks expressions that
     * designate an object.
     */
    Type *type;
    struct Symbol *sym;
    int lvalue;
    long temp;      /* set by code generation: frame offset of a temporary the node needs (a call returning a record) */
    union {
        char *name;
        char *text;
        ASTList *list;
        struct {
            ASTList *items;
            struct Scope *scope;
        } block;
        StorageKind storage;
        QualKind qual;
        TypeSpecKind type_spec;
        struct {
            OpKind op;
            AST *operand;
        } unary;
        struct {
            OpKind op;
            AST *left;
            AST *right;
        } binary;
        struct {
            AST *cond;
            AST *then;
            AST *otherwise;
        } cond;
        struct {
            AST *func;
            ASTList *args;
        } call;
        struct {
            AST *array;
            AST *index;
        } index;
        struct {
            AST *object;
            char *name;
            int arrow;
            struct Member *member;  /* resolved by sema */
        } member;
        struct {
            AST *type;
            AST *expr;
        } cast;
        struct {
            char *name;
            AST *expr;
            AST *body;
        } stmt;
        struct {
            AST *init;
            AST *cond;
            AST *step;
            AST *body;
        } loop;
        struct {
            ASTList *specs;
            AST *declarator;
            ASTList *kr_decls;
            AST *body;
            struct Scope *scope;  /* parameters and the outermost block */
        } func;
        struct {
            ASTList *specs;
            ASTList *declarators;
        } decl;
        struct {
            AST *declarator;
            AST *init;
        } init_decl;
        struct {
            ASTList *path;  /* DesigMember and DesigIndex nodes, outermost first */
            AST *init;
        } desig;
        struct {
            char *name;
            AST *inner;
            AST *size;
            ASTList *params;
            ASTList *quals;
            ASTList *attrs;
            int variadic;
            int kr;     /* params is a K&R identifier list */
            int block;  /* `^` rather than `*` */
            struct Scope *scope;  /* DeclFunc: the parameters' scope */
        } declarator;
        struct {
            char *tag;
            ASTList *members;
            ASTList *attrs;       /* __attribute__ after the keyword, may be NULL */
            ASTList *underlying;  /* enum: the specifiers after `:` (a fixed underlying type), may be NULL */
            int is_union;
            int has_body;
            int pack;             /* `#pragma pack` in effect at the body, 0 if none */
            struct Scope *scope;  /* members, when there is a body */
        } record;
        struct {
            char *name;
            AST *value;
            ASTList *attrs;
        } enumerator;
        struct {
            ASTList *specs;
            AST *declarator;
        } typed;
    } u;
};

/* lists */

ASTList *ast_list_new(void);
/* append; a NULL list is created first, so ast_list_push(NULL, n) works */
ASTList *ast_list_push(ASTList *list, AST *node);
ASTList *ast_list_prepend(ASTList *list, AST *node);
/* move every item of `tail` onto the end of `head` and free `tail` */
ASTList *ast_list_concat(ASTList *head, ASTList *tail);
void ast_list_free(ASTList *list);

/* nodes */

AST *ast_new(NodeKind kind, Loc loc);
void ast_free(AST *ast);

/* `name` and `text` are taken over by the node, not copied */
AST *ast_name(NodeKind kind, Loc loc, char *name);
AST *ast_text(NodeKind kind, Loc loc, char *text);
AST *ast_unary(Loc loc, OpKind op, AST *operand);
AST *ast_binary(NodeKind kind, Loc loc, OpKind op, AST *left, AST *right);
AST *ast_cond(NodeKind kind, Loc loc, AST *cond, AST *then, AST *otherwise);
AST *ast_stmt(NodeKind kind, Loc loc, AST *expr, AST *body);
AST *ast_list_node(NodeKind kind, Loc loc, ASTList *list);
AST *ast_declarator(NodeKind kind, Loc loc, AST *inner);
AST *ast_typed(NodeKind kind, Loc loc, ASTList *specs, AST *declarator);

/* wrap `*slot` in an ImplicitCast to `type` */
void ast_implicit_cast(AST **slot, Type *type);

/* `"a" "b"` becomes one String node; frees `right` */
AST *ast_string_concat(AST *left, AST *right);

/*
 * Graft `inner` onto the innermost end of a declarator chain (a pointer
 * chain built before its direct declarator was seen). Returns `chain`.
 */
AST *ast_declarator_attach(AST *chain, AST *inner);

/* the identifier a declarator declares, or NULL for an abstract declarator */
const char *ast_declarator_name(const AST *declarator);

/*
 * The node applied directly to the name: DeclFunc for `int f(void)`,
 * DeclPointer for `int (*f)(void)`, DeclArray for `int a[3]`. NULL for a
 * bare name.
 */
AST *ast_declarator_innermost(AST *declarator);

/* printable names */

const char *node_kind_name(NodeKind kind);
const char *op_name(OpKind op);
const char *storage_name(StorageKind storage);
const char *qual_name(QualKind qual);
const char *type_spec_name(TypeSpecKind type);

void ast_dump(const AST *ast, FILE *out);

#endif /* AST_H */
