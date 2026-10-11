#ifndef SYMTAB_H
#define SYMTAB_H

#include <stddef.h>
#include <stdio.h>

#include "ast.h"

/*
 * C has four name spaces. A name may be declared once per name space per
 * scope; the same spelling can be a typedef, a struct tag, a member and a
 * label all at once.
 */
typedef enum Namespace {
    NS_ORDINARY,    /* variables, functions, typedefs, enumerators, parameters */
    NS_TAG,         /* struct, union and enum tags */
    NS_MEMBER,      /* struct and union members, one name space per struct */
    NS_LABEL        /* goto labels, one name space per function */
} Namespace;

#define SYM_KINDS(X) \
    X(SYM_TYPEDEF, "typedef") \
    X(SYM_VARIABLE, "variable") \
    X(SYM_FUNCTION, "function") \
    X(SYM_PARAM, "param") \
    X(SYM_ENUMERATOR, "enumerator") \
    X(SYM_STRUCT, "struct tag") \
    X(SYM_UNION, "union tag") \
    X(SYM_ENUM, "enum tag") \
    X(SYM_MEMBER, "member") \
    X(SYM_LABEL, "label")

typedef enum SymKind {
#define X(name, repr) name,
    SYM_KINDS(X)
#undef X
    SYM_NKINDS
} SymKind;

#define SCOPE_KINDS(X) \
    X(SCOPE_FILE, "file") \
    X(SCOPE_FUNCTION, "function")   /* parameters, labels and the outermost block */ \
    X(SCOPE_BLOCK, "block") \
    X(SCOPE_PROTOTYPE, "prototype") /* parameters of a declarator that is not a definition */ \
    X(SCOPE_STRUCT, "struct")       /* members; ordinary names declared inside go to the enclosing scope */

typedef enum ScopeKind {
#define X(name, repr) name,
    SCOPE_KINDS(X)
#undef X
    SCOPE_NKINDS
} ScopeKind;

typedef struct Scope Scope;
typedef struct Symbol Symbol;

struct Symbol {
    char *name;
    Namespace ns;
    SymKind kind;
    AST *decl;      /* the node that declares it (InitDeclarator, Param, Enumerator, ...) */
    AST *def;       /* the definition, once seen: FunctionDef, StructSpec or EnumSpec with a body */
    Loc loc;
    Scope *scope;   /* the scope it is declared in */
    Symbol *next;   /* next symbol declared in the same scope */
    Symbol *chain;  /* hash chain */
    /* set by semantic analysis */
    Type *type;
    StorageKind storage;    /* STORAGE_NONE when there was no specifier */
    long value;        /* enumerators */
    /* set by code generation (src/codegen.c) */
    long offset;            /* locals and parameters: bytes from the frame pointer */
    int vreg;               /* a local kept in an IR virtual register: its number plus 1; -1 while lowering marks the ones whose address is taken */
    int label;              /* labels: jump target number; static locals: suffix making the assembler name unique */
    int by_ref;             /* a record parameter passed by reference: the slot holds its address */
    int used;               /* functions: referenced from code that is emitted */
    int addr_taken;         /* functions: referenced other than as the callee of a direct call (by emitted code or an initializer) */
};

struct Scope {
    ScopeKind kind;
    char *name;     /* function name or struct tag, for dumps; may be NULL */
    Loc loc;        /* where the scope opens */
    Scope *parent;
    Scope *first_child;
    Scope *last_child;
    Scope *next_sibling;
    Symbol *first;  /* symbols in declaration order */
    Symbol *last;
    Symbol **buckets;
    size_t nbuckets;
    size_t count;
};

/*
 * Scopes are never freed on pop: AST nodes point at them, and later passes
 * re-enter them. Everything is released by symtab_free.
 */
typedef struct SymTab {
    Scope *file;
    Scope *current;
} SymTab;

SymTab *symtab_new(void);
void symtab_free(SymTab *st);

/* open a new scope under the current one and make it current */
Scope *symtab_push(SymTab *st, ScopeKind kind, const char *name, Loc loc);
/* close the current scope; returns it */
Scope *symtab_pop(SymTab *st);
/* make a previously popped child of the current scope current again */
void symtab_enter(SymTab *st, Scope *scope);
/* remove an empty, just-popped scope from the tree and free it */
void symtab_discard(SymTab *st, Scope *scope);
/* nearest enclosing function scope, or NULL at file scope */
Scope *symtab_function_scope(const SymTab *st);

/*
 * Declare `name` in the scope that `ns` lives in: members in the current
 * struct scope, labels in the function scope, everything else in the
 * nearest scope that is not a struct body. If the name is already declared
 * there in that name space, the existing symbol is returned unchanged.
 * `name` is copied.
 */
Symbol *symtab_declare(SymTab *st, Namespace ns, SymKind kind,
                       const char *name, AST *decl, Loc loc);

/* look through the current scope and all enclosing ones */
Symbol *symtab_lookup(const SymTab *st, Namespace ns, const char *name);
/* look in one scope only */
Symbol *scope_lookup(const Scope *scope, Namespace ns, const char *name);

const char *sym_kind_name(SymKind kind);
const char *scope_kind_name(ScopeKind kind);
const char *namespace_name(Namespace ns);

void symtab_dump(const SymTab *st, FILE *out);

#endif /* SYMTAB_H */
